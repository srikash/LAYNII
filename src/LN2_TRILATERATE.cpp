
#include "../dep/laynii_lib.h"
#include <limits>
#include <sstream>

int show_help(void) {
    printf(
    "LN2_TRILATERATE: TODO\n"
    "\n"
    "Usage:\n"
    "    LN2_TRILATERATE -rim rim.nii -midgm rim_midgm_equidist.nii -centroid rim_midgm_centroid.nii\n"
    "\n"
    "Options:\n"
    "    -help       : Show this help.\n"
    "    -rim        : Segmentation input. Use 3 to code pure gray matter \n"
    "                  voxels. This program only generates columns in the \n"
    "                  voxels coded with 3.\n"
    "    -midgm      : Middle gray matter file (from LN2_LAYERS output).\n"
    "    -centroid   : (Optional) One voxel for centroid (use 1).\n"
    "    -debug      : (Optional) Save extra intermediate outputs.\n"
    "    -output     : (Optional) Output basename for all outputs.\n"
    "\n");
    return 0;
}

int main(int argc, char*  argv[]) {

    nifti_image *nii1 = NULL, *nii2 = NULL, *nii3 = NULL;
    char *fin1 = NULL, *fout = NULL, *fin2=NULL, *fin3=NULL;
    int ac;
    bool mode_debug = false;

    // Process user options
    if (argc < 2) return show_help();
    for (ac = 1; ac < argc; ac++) {
        if (!strncmp(argv[ac], "-h", 2)) {
            return show_help();
        } else if (!strcmp(argv[ac], "-rim")) {
            if (++ac >= argc) {
                fprintf(stderr, "** missing argument for -rim\n");
                return 1;
            }
            fin1 = argv[ac];
            fout = argv[ac];
        } else if (!strcmp(argv[ac], "-midgm")) {
            if (++ac >= argc) {
                fprintf(stderr, "** missing argument for -midgm\n");
                return 1;
            }
            fin2 = argv[ac];
        } else if (!strcmp(argv[ac], "-centroid")) {
            if (++ac >= argc) {
                fprintf(stderr, "** missing argument for -centroid\n");
                return 1;
            }
            fin3 = argv[ac];
        } else if (!strcmp(argv[ac], "-output")) {
            if (++ac >= argc) {
                fprintf(stderr, "** missing argument for -output\n");
                return 1;
            }
            fout = argv[ac];
        } else if (!strcmp(argv[ac], "-debug")) {
            mode_debug = true;
        } else {
            fprintf(stderr, "** invalid option, '%s'\n", argv[ac]);
            return 1;
        }
    }

    if (!fin1) {
        fprintf(stderr, "** missing option '-rim'\n");
        return 1;
    }
    if (!fin2) {
        fprintf(stderr, "** missing option '-midgm'\n");
        return 1;
    }
    if (!fin3) {
        fprintf(stderr, "** missing option '-centroid'\n");
        return 1;
    }

    // Read input dataset, including data
    nii1 = nifti_image_read(fin1, 1);
    if (!nii1) {
        fprintf(stderr, "** failed to read NIfTI from '%s'\n", fin1);
        return 2;
    }
    nii2 = nifti_image_read(fin2, 1);
    if (!nii2) {
        fprintf(stderr, "** failed to read NIfTI from '%s'\n", fin2);
        return 2;
    }
    nii3 = nifti_image_read(fin3, 1);
    if (!nii3) {
        fprintf(stderr, "** failed to read NIfTI from '%s'\n", fin2);
        return 2;
    }

    log_welcome("LN2_TRILATERATE");
    log_nifti_descriptives(nii1);
    log_nifti_descriptives(nii2);
    log_nifti_descriptives(nii3);

    // Get dimensions of input
    const uint32_t size_x = nii1->nx;
    const uint32_t size_y = nii1->ny;
    const uint32_t size_z = nii1->nz;

    const uint32_t end_x = size_x - 1;
    const uint32_t end_y = size_y - 1;
    const uint32_t end_z = size_z - 1;

    const uint32_t nr_voxels = size_z * size_y * size_x;

    const float dX = nii1->pixdim[1];
    const float dY = nii1->pixdim[2];
    const float dZ = nii1->pixdim[3];

    // Short diagonals
    const float dia_xy = sqrt(dX * dX + dY * dY);
    const float dia_xz = sqrt(dX * dX + dZ * dZ);
    const float dia_yz = sqrt(dY * dY + dZ * dZ);
    // Long diagonals
    const float dia_xyz = sqrt(dX * dX + dY * dY + dZ * dZ);

    // ========================================================================
    // Fix input datatype issues
    nifti_image* nii_rim = copy_nifti_as_int32(nii1);
    int32_t* nii_rim_data = static_cast<int32_t*>(nii_rim->data);
    nifti_image* nii_midgm = copy_nifti_as_int32(nii2);
    int32_t* nii_midgm_data = static_cast<int32_t*>(nii_midgm->data);
    nifti_image* nii_centroid = copy_nifti_as_int32(nii3);
    int32_t* nii_centroid_data = static_cast<int32_t*>(nii_centroid->data);

    // Prepare required nifti images
    nifti_image* triplet = copy_nifti_as_int32(nii_rim);
    int32_t* triplet_data = static_cast<int32_t*>(triplet->data);
    nifti_image* flood_step = copy_nifti_as_int32(nii_rim);
    int32_t* flood_step_data = static_cast<int32_t*>(flood_step->data);
    nifti_image* flood_dist = copy_nifti_as_float32(nii_rim);
    float* flood_dist_data = static_cast<float*>(flood_dist->data);

    nifti_image* perimeter = copy_nifti_as_int32(nii_rim);
    int32_t* perimeter_data = static_cast<int32_t*>(perimeter->data);

    // Setting zero
    for (uint32_t i = 0; i != nr_voxels; ++i) {
        *(triplet_data + i) = 0;
        *(flood_step_data + i) = 0;
        *(flood_dist_data + i) = 0;
        *(perimeter_data + i) = 0;
    }

    // ------------------------------------------------------------------------
    // NOTE(Faruk): This section is written to constrain the big iterative
    // flooding distance loop to the subset of voxels. Required for substantial
    // speed boost.
    // Find the subset voxels that will be used many times
    uint32_t nr_voi = 0;  // Voxels of interest
    for (uint32_t i = 0; i != nr_voxels; ++i) {
        if (*(nii_midgm_data + i) == 1){
            nr_voi += 1;
        }
    }
    // Allocate memory to only the voxel of interest
    int32_t* voi_id;
    voi_id = (int32_t*) malloc(nr_voi*sizeof(int32_t));

    // Fill in indices to be able to remap from subset to full set of voxels
    uint32_t ii = 0;
    for (uint32_t i = 0; i != nr_voxels; ++i) {
        if (*(nii_midgm_data + i) == 1){
            *(voi_id + ii) = i;
            ii += 1;
        }
    }

    // ========================================================================
    // Find column centers through farthest flood distance
    // ========================================================================
    cout << "  Start trilaterating..." << endl;
    // Find the initial voxel
    uint32_t start_voxel;
    for (uint32_t i = 0; i != nr_voxels; ++i) {
        if (*(nii_centroid_data + i) == 1) {
            start_voxel = i;
        }
    }
    *(nii_midgm_data + start_voxel) = 2;

    // Find distances from input centroid
    float flood_dist_thr = std::numeric_limits<float>::infinity();
    int32_t grow_step = 1;
    uint32_t voxel_counter = nr_voxels;
    uint32_t ix, iy, iz, i, j;
    float d;

    // Initialize grow volume
    for (uint32_t i = 0; i != nr_voxels; ++i) {
        if (*(nii_midgm_data + i) == 2) {
            *(flood_step_data + i) = 1.;
            *(flood_dist_data + i) = 0.;
        } else if (*(flood_dist_data + i) >= flood_dist_thr
                   && *(flood_dist_data + i) > 0) {
            *(flood_step_data + i) = 0.;
            *(flood_dist_data + i) = 0.;
            *(nii_midgm_data + i) = 1;
        } else if (*(flood_dist_data + i) < flood_dist_thr
                   && *(flood_dist_data + i) > 0) {
            *(nii_midgm_data + i) = 0;  // no need to recompute
        }
    }

    while (voxel_counter != 0) {
        voxel_counter = 0;
        for (uint32_t ii = 0; ii != nr_voi; ++ii) {
            i = *(voi_id + ii);  // Map subset to full set
            if (*(flood_step_data + i) == grow_step) {
                tie(ix, iy, iz) = ind2sub_3D(i, size_x, size_y);
                voxel_counter += 1;

                // --------------------------------------------------------
                // 1-jump neighbours
                // --------------------------------------------------------
                if (ix > 0) {
                    j = sub2ind_3D(ix-1, iy, iz, size_x, size_y);
                    if (*(nii_midgm_data + j) == 1) {
                        d = *(flood_dist_data + i) + dX;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (ix < end_x) {
                    j = sub2ind_3D(ix+1, iy, iz, size_x, size_y);
                    if (*(nii_midgm_data + j) == 1) {
                        d = *(flood_dist_data + i) + dX;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (iy > 0) {
                    j = sub2ind_3D(ix, iy-1, iz, size_x, size_y);
                    if (*(nii_midgm_data + j) == 1) {
                        d = *(flood_dist_data + i) + dY;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (iy < end_y) {
                    j = sub2ind_3D(ix, iy+1, iz, size_x, size_y);
                    if (*(nii_midgm_data + j) == 1) {
                        d = *(flood_dist_data + i) + dY;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (iz > 0) {
                    j = sub2ind_3D(ix, iy, iz-1, size_x, size_y);
                    if (*(nii_midgm_data + j) == 1) {
                        d = *(flood_dist_data + i) + dZ;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (iz < end_z) {
                    j = sub2ind_3D(ix, iy, iz+1, size_x, size_y);

                    if (*(nii_midgm_data + j) == 1) {
                        d = *(flood_dist_data + i) + dZ;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                // --------------------------------------------------------
                // 2-jump neighbours
                // --------------------------------------------------------
                if (ix > 0 && iy > 0) {
                    j = sub2ind_3D(ix-1, iy-1, iz, size_x, size_y);

                    if (*(nii_midgm_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_xy;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (ix > 0 && iy < end_y) {
                    j = sub2ind_3D(ix-1, iy+1, iz, size_x, size_y);

                    if (*(nii_midgm_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_xy;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (ix < end_x && iy > 0) {
                    j = sub2ind_3D(ix+1, iy-1, iz, size_x, size_y);

                    if (*(nii_midgm_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_xy;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (ix < end_x && iy < end_y) {
                    j = sub2ind_3D(ix+1, iy+1, iz, size_x, size_y);

                    if (*(nii_midgm_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_xy;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (iy > 0 && iz > 0) {
                    j = sub2ind_3D(ix, iy-1, iz-1, size_x, size_y);

                    if (*(nii_midgm_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_yz;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (iy > 0 && iz < end_z) {
                    j = sub2ind_3D(ix, iy-1, iz+1, size_x, size_y);

                    if (*(nii_midgm_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_yz;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (iy < end_y && iz > 0) {
                    j = sub2ind_3D(ix, iy+1, iz-1, size_x, size_y);

                    if (*(nii_midgm_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_yz;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (iy < end_y && iz < end_z) {
                    j = sub2ind_3D(ix, iy+1, iz+1, size_x, size_y);

                    if (*(nii_midgm_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_yz;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (ix > 0 && iz > 0) {
                    j = sub2ind_3D(ix-1, iy, iz-1, size_x, size_y);

                    if (*(nii_midgm_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_xz;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (ix < end_x && iz > 0) {
                    j = sub2ind_3D(ix+1, iy, iz-1, size_x, size_y);

                    if (*(nii_midgm_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_xz;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (ix > 0 && iz < end_z) {
                    j = sub2ind_3D(ix-1, iy, iz+1, size_x, size_y);

                    if (*(nii_midgm_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_xz;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (ix < end_x && iz < end_z) {
                    j = sub2ind_3D(ix+1, iy, iz+1, size_x, size_y);

                    if (*(nii_midgm_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_xz;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }

                // --------------------------------------------------------
                // 3-jump neighbours
                // --------------------------------------------------------
                if (ix > 0 && iy > 0 && iz > 0) {
                    j = sub2ind_3D(ix-1, iy-1, iz-1, size_x, size_y);

                    if (*(nii_midgm_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_xyz;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (ix > 0 && iy > 0 && iz < end_z) {
                    j = sub2ind_3D(ix-1, iy-1, iz+1, size_x, size_y);

                    if (*(nii_midgm_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_xyz;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (ix > 0 && iy < end_y && iz > 0) {
                    j = sub2ind_3D(ix-1, iy+1, iz-1, size_x, size_y);

                    if (*(nii_midgm_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_xyz;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (ix < end_x && iy > 0 && iz > 0) {
                    j = sub2ind_3D(ix+1, iy-1, iz-1, size_x, size_y);

                    if (*(nii_midgm_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_xyz;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (ix > 0 && iy < end_y && iz < end_z) {
                    j = sub2ind_3D(ix-1, iy+1, iz+1, size_x, size_y);

                    if (*(nii_midgm_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_xyz;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (ix < end_x && iy > 0 && iz < end_z) {
                    j = sub2ind_3D(ix+1, iy-1, iz+1, size_x, size_y);

                    if (*(nii_midgm_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_xyz;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (ix < end_x && iy < end_y && iz > 0) {
                    j = sub2ind_3D(ix+1, iy+1, iz-1, size_x, size_y);

                    if (*(nii_midgm_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_xyz;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (ix < end_x && iy < end_y && iz < end_z) {
                    j = sub2ind_3D(ix+1, iy+1, iz+1, size_x, size_y);

                    if (*(nii_midgm_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_xyz;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
            }
        }
        grow_step += 1;
    }

    // Translate 0 crossing
    float RADIUS = 2;
    for (uint32_t ii = 0; ii != nr_voi; ++ii) {
        i = *(voi_id + ii);
        *(flood_dist_data + i) -= RADIUS;
    }
    // save_output_nifti(fout, "flood_dist", flood_dist, false);

    // ========================================================================
    // Find perimeter
    // ========================================================================
    cout << "\n  Start finding perimeter..." << endl;

    // TODO(Faruk): Once the proof of concept is working, I need to reduce code
    // repetition here through functions. Signbit checks are easy reductions.

    for (uint32_t ii = 0; ii != nr_voi; ++ii) {
        i = *(voi_id + ii);  // Map subset to full set
        // Check sign changes to find zero crossings
        if (*(flood_dist_data + i) == 0) {
            *(perimeter_data + i) = 1;
        } else {
            float m = *(flood_dist_data + i);
            float n;
            tie(ix, iy, iz) = ind2sub_3D(i, size_x, size_y);

            // --------------------------------------------------------
            // 1-jump neighbours
            // --------------------------------------------------------
            if (ix > 0) {
                j = sub2ind_3D(ix-1, iy, iz, size_x, size_y);
                if (*(nii_midgm_data + j) == 1){
                    n = *(flood_dist_data + j);
                    if (signbit(m) - signbit(n) != 0) {
                        if (m*m < n*n) {
                            *(perimeter_data + i) = 1;
                        } else if (m*m > n*n) {  // Closer to prev. step
                            *(perimeter_data + j) = 1;
                        } else {  // Equal +/- normalized distance
                            *(perimeter_data + i) = 1;
                            *(perimeter_data + j) = 1;
                        }
                    }
                }
            }
            if (ix < end_x) {
                j = sub2ind_3D(ix-1, iy, iz, size_x, size_y);
                if (*(nii_midgm_data + j) == 1){
                    n = *(flood_dist_data + j);
                    if (signbit(m) - signbit(n) != 0) {
                        if (m*m < n*n) {
                            *(perimeter_data + i) = 1;
                        } else if (m*m > n*n) {  // Closer to prev. step
                            *(perimeter_data + j) = 1;
                        } else {  // Equal +/- normalized distance
                            *(perimeter_data + i) = 1;
                            *(perimeter_data + j) = 1;
                        }
                    }
                }
            }
            if (iy > 0) {
                j = sub2ind_3D(ix-1, iy, iz, size_x, size_y);
                if (*(nii_midgm_data + j) == 1){
                    n = *(flood_dist_data + j);
                    if (signbit(m) - signbit(n) != 0) {
                        if (m*m < n*n) {
                            *(perimeter_data + i) = 1;
                        } else if (m*m > n*n) {  // Closer to prev. step
                            *(perimeter_data + j) = 1;
                        } else {  // Equal +/- normalized distance
                            *(perimeter_data + i) = 1;
                            *(perimeter_data + j) = 1;
                        }
                    }
                }
            }
            if (iy < end_y) {
                j = sub2ind_3D(ix-1, iy, iz, size_x, size_y);
                if (*(nii_midgm_data + j) == 1){
                    n = *(flood_dist_data + j);
                    if (signbit(m) - signbit(n) != 0) {
                        if (m*m < n*n) {
                            *(perimeter_data + i) = 1;
                        } else if (m*m > n*n) {  // Closer to prev. step
                            *(perimeter_data + j) = 1;
                        } else {  // Equal +/- normalized distance
                            *(perimeter_data + i) = 1;
                            *(perimeter_data + j) = 1;
                        }
                    }
                }
            }
            if (iz > 0) {
                j = sub2ind_3D(ix-1, iy, iz, size_x, size_y);
                if (*(nii_midgm_data + j) == 1){
                    n = *(flood_dist_data + j);
                    if (signbit(m) - signbit(n) != 0) {
                        if (m*m < n*n) {
                            *(perimeter_data + i) = 1;
                        } else if (m*m > n*n) {  // Closer to prev. step
                            *(perimeter_data + j) = 1;
                        } else {  // Equal +/- normalized distance
                            *(perimeter_data + i) = 1;
                            *(perimeter_data + j) = 1;
                        }
                    }
                }
            }
            if (iz < end_z) {
                j = sub2ind_3D(ix-1, iy, iz, size_x, size_y);
                if (*(nii_midgm_data + j) == 1){
                    n = *(flood_dist_data + j);
                    if (signbit(m) - signbit(n) != 0) {
                        if (m*m < n*n) {
                            *(perimeter_data + i) = 1;
                        } else if (m*m > n*n) {  // Closer to prev. step
                            *(perimeter_data + j) = 1;
                        } else {  // Equal +/- normalized distance
                            *(perimeter_data + i) = 1;
                            *(perimeter_data + j) = 1;
                        }
                    }
                }
            }
            // --------------------------------------------------------
            // 2-jump neighbours
            // --------------------------------------------------------
            if (ix > 0 && iy > 0) {
                j = sub2ind_3D(ix-1, iy-1, iz, size_x, size_y);
                if (*(nii_midgm_data + j) == 1){
                    n = *(flood_dist_data + j);
                    if (signbit(m) - signbit(n) != 0) {
                        if (m*m < n*n) {
                            *(perimeter_data + i) = 1;
                        } else if (m*m > n*n) {  // Closer to prev. step
                            *(perimeter_data + j) = 1;
                        } else {  // Equal +/- normalized distance
                            *(perimeter_data + i) = 1;
                            *(perimeter_data + j) = 1;
                        }
                    }
                }
            }
            if (ix > 0 && iy < end_y) {
                j = sub2ind_3D(ix-1, iy+1, iz, size_x, size_y);
                if (*(nii_midgm_data + j) == 1){
                    n = *(flood_dist_data + j);
                    if (signbit(m) - signbit(n) != 0) {
                        if (m*m < n*n) {
                            *(perimeter_data + i) = 1;
                        } else if (m*m > n*n) {  // Closer to prev. step
                            *(perimeter_data + j) = 1;
                        } else {  // Equal +/- normalized distance
                            *(perimeter_data + i) = 1;
                            *(perimeter_data + j) = 1;
                        }
                    }
                }
            }
            if (ix < end_x && iy > 0) {
                j = sub2ind_3D(ix+1, iy-1, iz, size_x, size_y);
                if (*(nii_midgm_data + j) == 1){
                    n = *(flood_dist_data + j);
                    if (signbit(m) - signbit(n) != 0) {
                        if (m*m < n*n) {
                            *(perimeter_data + i) = 1;
                        } else if (m*m > n*n) {  // Closer to prev. step
                            *(perimeter_data + j) = 1;
                        } else {  // Equal +/- normalized distance
                            *(perimeter_data + i) = 1;
                            *(perimeter_data + j) = 1;
                        }
                    }
                }
            }
            if (ix < end_x && iy < end_y) {
                j = sub2ind_3D(ix+1, iy+1, iz, size_x, size_y);
                if (*(nii_midgm_data + j) == 1){
                    n = *(flood_dist_data + j);
                    if (signbit(m) - signbit(n) != 0) {
                        if (m*m < n*n) {
                            *(perimeter_data + i) = 1;
                        } else if (m*m > n*n) {  // Closer to prev. step
                            *(perimeter_data + j) = 1;
                        } else {  // Equal +/- normalized distance
                            *(perimeter_data + i) = 1;
                            *(perimeter_data + j) = 1;
                        }
                    }
                }
            }
            if (iy > 0 && iz > 0) {
                j = sub2ind_3D(ix, iy-1, iz-1, size_x, size_y);
                if (*(nii_midgm_data + j) == 1){
                    n = *(flood_dist_data + j);
                    if (signbit(m) - signbit(n) != 0) {
                        if (m*m < n*n) {
                            *(perimeter_data + i) = 1;
                        } else if (m*m > n*n) {  // Closer to prev. step
                            *(perimeter_data + j) = 1;
                        } else {  // Equal +/- normalized distance
                            *(perimeter_data + i) = 1;
                            *(perimeter_data + j) = 1;
                        }
                    }
                }
            }
            if (iy > 0 && iz < end_z) {
                j = sub2ind_3D(ix, iy-1, iz+1, size_x, size_y);
                if (*(nii_midgm_data + j) == 1){
                    n = *(flood_dist_data + j);
                    if (signbit(m) - signbit(n) != 0) {
                        if (m*m < n*n) {
                            *(perimeter_data + i) = 1;
                        } else if (m*m > n*n) {  // Closer to prev. step
                            *(perimeter_data + j) = 1;
                        } else {  // Equal +/- normalized distance
                            *(perimeter_data + i) = 1;
                            *(perimeter_data + j) = 1;
                        }
                    }
                }
            }
            if (iy < end_y && iz > 0) {
                j = sub2ind_3D(ix, iy+1, iz-1, size_x, size_y);
                if (*(nii_midgm_data + j) == 1){
                    n = *(flood_dist_data + j);
                    if (signbit(m) - signbit(n) != 0) {
                        if (m*m < n*n) {
                            *(perimeter_data + i) = 1;
                        } else if (m*m > n*n) {  // Closer to prev. step
                            *(perimeter_data + j) = 1;
                        } else {  // Equal +/- normalized distance
                            *(perimeter_data + i) = 1;
                            *(perimeter_data + j) = 1;
                        }
                    }
                }
            }
            if (iy < end_y && iz < end_z) {
                j = sub2ind_3D(ix, iy+1, iz+1, size_x, size_y);
                if (*(nii_midgm_data + j) == 1){
                    n = *(flood_dist_data + j);
                    if (signbit(m) - signbit(n) != 0) {
                        if (m*m < n*n) {
                            *(perimeter_data + i) = 1;
                        } else if (m*m > n*n) {  // Closer to prev. step
                            *(perimeter_data + j) = 1;
                        } else {  // Equal +/- normalized distance
                            *(perimeter_data + i) = 1;
                            *(perimeter_data + j) = 1;
                        }
                    }
                }
            }
            if (ix > 0 && iz > 0) {
                j = sub2ind_3D(ix-1, iy, iz-1, size_x, size_y);
                if (*(nii_midgm_data + j) == 1){
                    n = *(flood_dist_data + j);
                    if (signbit(m) - signbit(n) != 0) {
                        if (m*m < n*n) {
                            *(perimeter_data + i) = 1;
                        } else if (m*m > n*n) {  // Closer to prev. step
                            *(perimeter_data + j) = 1;
                        } else {  // Equal +/- normalized distance
                            *(perimeter_data + i) = 1;
                            *(perimeter_data + j) = 1;
                        }
                    }
                }
            }
            if (ix < end_x && iz > 0) {
                j = sub2ind_3D(ix+1, iy, iz-1, size_x, size_y);
                if (*(nii_midgm_data + j) == 1){
                    n = *(flood_dist_data + j);
                    if (signbit(m) - signbit(n) != 0) {
                        if (m*m < n*n) {
                            *(perimeter_data + i) = 1;
                        } else if (m*m > n*n) {  // Closer to prev. step
                            *(perimeter_data + j) = 1;
                        } else {  // Equal +/- normalized distance
                            *(perimeter_data + i) = 1;
                            *(perimeter_data + j) = 1;
                        }
                    }
                }
            }
            if (ix > 0 && iz < end_z) {
                j = sub2ind_3D(ix-1, iy, iz+1, size_x, size_y);
                if (*(nii_midgm_data + j) == 1){
                    n = *(flood_dist_data + j);
                    if (signbit(m) - signbit(n) != 0) {
                        if (m*m < n*n) {
                            *(perimeter_data + i) = 1;
                        } else if (m*m > n*n) {  // Closer to prev. step
                            *(perimeter_data + j) = 1;
                        } else {  // Equal +/- normalized distance
                            *(perimeter_data + i) = 1;
                            *(perimeter_data + j) = 1;
                        }
                    }
                }
            }
            if (ix < end_x && iz < end_z) {
                j = sub2ind_3D(ix+1, iy, iz+1, size_x, size_y);
                if (*(nii_midgm_data + j) == 1){
                    n = *(flood_dist_data + j);
                    if (signbit(m) - signbit(n) != 0) {
                        if (m*m < n*n) {
                            *(perimeter_data + i) = 1;
                        } else if (m*m > n*n) {  // Closer to prev. step
                            *(perimeter_data + j) = 1;
                        } else {  // Equal +/- normalized distance
                            *(perimeter_data + i) = 1;
                            *(perimeter_data + j) = 1;
                        }
                    }
                }
            }
            // --------------------------------------------------------
            // 3-jump neighbours
            // --------------------------------------------------------
            if (ix > 0 && iy > 0 && iz > 0) {
                j = sub2ind_3D(ix-1, iy-1, iz-1, size_x, size_y);
                if (*(nii_midgm_data + j) == 1){
                    n = *(flood_dist_data + j);
                    if (signbit(m) - signbit(n) != 0) {
                        if (m*m < n*n) {
                            *(perimeter_data + i) = 1;
                        } else if (m*m > n*n) {  // Closer to prev. step
                            *(perimeter_data + j) = 1;
                        } else {  // Equal +/- normalized distance
                            *(perimeter_data + i) = 1;
                            *(perimeter_data + j) = 1;
                        }
                    }
                }
            }
            if (ix > 0 && iy > 0 && iz < end_z) {
                j = sub2ind_3D(ix-1, iy-1, iz+1, size_x, size_y);
                if (*(nii_midgm_data + j) == 1){
                    n = *(flood_dist_data + j);
                    if (signbit(m) - signbit(n) != 0) {
                        if (m*m < n*n) {
                            *(perimeter_data + i) = 1;
                        } else if (m*m > n*n) {  // Closer to prev. step
                            *(perimeter_data + j) = 1;
                        } else {  // Equal +/- normalized distance
                            *(perimeter_data + i) = 1;
                            *(perimeter_data + j) = 1;
                        }
                    }
                }
            }
            if (ix > 0 && iy < end_y && iz > 0) {
                j = sub2ind_3D(ix-1, iy+1, iz-1, size_x, size_y);
                if (*(nii_midgm_data + j) == 1){
                    n = *(flood_dist_data + j);
                    if (signbit(m) - signbit(n) != 0) {
                        if (m*m < n*n) {
                            *(perimeter_data + i) = 1;
                        } else if (m*m > n*n) {  // Closer to prev. step
                            *(perimeter_data + j) = 1;
                        } else {  // Equal +/- normalized distance
                            *(perimeter_data + i) = 1;
                            *(perimeter_data + j) = 1;
                        }
                    }
                }
            }
            if (ix < end_x && iy > 0 && iz > 0) {
                j = sub2ind_3D(ix+1, iy-1, iz-1, size_x, size_y);
                if (*(nii_midgm_data + j) == 1){
                    n = *(flood_dist_data + j);
                    if (signbit(m) - signbit(n) != 0) {
                        if (m*m < n*n) {
                            *(perimeter_data + i) = 1;
                        } else if (m*m > n*n) {  // Closer to prev. step
                            *(perimeter_data + j) = 1;
                        } else {  // Equal +/- normalized distance
                            *(perimeter_data + i) = 1;
                            *(perimeter_data + j) = 1;
                        }
                    }
                }
            }
            if (ix > 0 && iy < end_y && iz < end_z) {
                j = sub2ind_3D(ix-1, iy+1, iz+1, size_x, size_y);
                if (*(nii_midgm_data + j) == 1){
                    n = *(flood_dist_data + j);
                    if (signbit(m) - signbit(n) != 0) {
                        if (m*m < n*n) {
                            *(perimeter_data + i) = 1;
                        } else if (m*m > n*n) {  // Closer to prev. step
                            *(perimeter_data + j) = 1;
                        } else {  // Equal +/- normalized distance
                            *(perimeter_data + i) = 1;
                            *(perimeter_data + j) = 1;
                        }
                    }
                }
            }
            if (ix < end_x && iy > 0 && iz < end_z) {
                j = sub2ind_3D(ix+1, iy-1, iz+1, size_x, size_y);
                if (*(nii_midgm_data + j) == 1){
                    n = *(flood_dist_data + j);
                    if (signbit(m) - signbit(n) != 0) {
                        if (m*m < n*n) {
                            *(perimeter_data + i) = 1;
                        } else if (m*m > n*n) {  // Closer to prev. step
                            *(perimeter_data + j) = 1;
                        } else {  // Equal +/- normalized distance
                            *(perimeter_data + i) = 1;
                            *(perimeter_data + j) = 1;
                        }
                    }
                }
            }
            if (ix < end_x && iy < end_y && iz > 0) {
                j = sub2ind_3D(ix+1, iy+1, iz-1, size_x, size_y);
                if (*(nii_midgm_data + j) == 1){
                    n = *(flood_dist_data + j);
                    if (signbit(m) - signbit(n) != 0) {
                        if (m*m < n*n) {
                            *(perimeter_data + i) = 1;
                        } else if (m*m > n*n) {  // Closer to prev. step
                            *(perimeter_data + j) = 1;
                        } else {  // Equal +/- normalized distance
                            *(perimeter_data + i) = 1;
                            *(perimeter_data + j) = 1;
                        }
                    }
                }
            }
            if (ix < end_x && iy < end_y && iz < end_z) {
                j = sub2ind_3D(ix+1, iy+1, iz+1, size_x, size_y);
                if (*(nii_midgm_data + j) == 1){
                    n = *(flood_dist_data + j);
                    if (signbit(m) - signbit(n) != 0) {
                        if (m*m < n*n) {
                            *(perimeter_data + i) = 1;
                        } else if (m*m > n*n) {  // Closer to prev. step
                            *(perimeter_data + j) = 1;
                        } else {  // Equal +/- normalized distance
                            *(perimeter_data + i) = 1;
                            *(perimeter_data + j) = 1;
                        }
                    }
                }
            }
        }
    }
    save_output_nifti(fout, "perimeter", perimeter, true);

    // ========================================================================
    // Find triplets and compute distances on midgm domain
    // ========================================================================
    // Find first point on perimeter
    int idx_point1;
    for (uint32_t ii = 0; ii != nr_voi; ++ii) {
        i = *(voi_id + ii);  // Map subset to full set
        if (*(perimeter_data + i) == 1) {
            idx_point1 = i;
        }
    }
    *(triplet_data + idx_point1) = 1;

    // Initialize grow volume
    for (uint32_t i = 0; i != nr_voxels; ++i) {
        *(flood_step_data + i) = 0.;
        *(flood_dist_data + i) = 0.;
    }
    *(flood_step_data + idx_point1) = 1.;

    // Reset some parameters
    flood_dist_thr = std::numeric_limits<float>::infinity();
    grow_step = 1;
    voxel_counter = nr_voxels;

    while (voxel_counter != 0) {
        voxel_counter = 0;
        for (uint32_t ii = 0; ii != nr_voi; ++ii) {
            i = *(voi_id + ii);  // Map subset to full set
            if (*(flood_step_data + i) == grow_step) {
                tie(ix, iy, iz) = ind2sub_3D(i, size_x, size_y);
                voxel_counter += 1;

                // --------------------------------------------------------
                // 1-jump neighbours
                // --------------------------------------------------------
                if (ix > 0) {
                    j = sub2ind_3D(ix-1, iy, iz, size_x, size_y);
                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dX;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (ix < end_x) {
                    j = sub2ind_3D(ix+1, iy, iz, size_x, size_y);
                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dX;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (iy > 0) {
                    j = sub2ind_3D(ix, iy-1, iz, size_x, size_y);
                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dY;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (iy < end_y) {
                    j = sub2ind_3D(ix, iy+1, iz, size_x, size_y);
                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dY;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (iz > 0) {
                    j = sub2ind_3D(ix, iy, iz-1, size_x, size_y);
                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dZ;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (iz < end_z) {
                    j = sub2ind_3D(ix, iy, iz+1, size_x, size_y);

                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dZ;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                // --------------------------------------------------------
                // 2-jump neighbours
                // --------------------------------------------------------
                if (ix > 0 && iy > 0) {
                    j = sub2ind_3D(ix-1, iy-1, iz, size_x, size_y);

                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_xy;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (ix > 0 && iy < end_y) {
                    j = sub2ind_3D(ix-1, iy+1, iz, size_x, size_y);

                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_xy;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (ix < end_x && iy > 0) {
                    j = sub2ind_3D(ix+1, iy-1, iz, size_x, size_y);

                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_xy;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (ix < end_x && iy < end_y) {
                    j = sub2ind_3D(ix+1, iy+1, iz, size_x, size_y);

                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_xy;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (iy > 0 && iz > 0) {
                    j = sub2ind_3D(ix, iy-1, iz-1, size_x, size_y);

                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_yz;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (iy > 0 && iz < end_z) {
                    j = sub2ind_3D(ix, iy-1, iz+1, size_x, size_y);

                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_yz;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (iy < end_y && iz > 0) {
                    j = sub2ind_3D(ix, iy+1, iz-1, size_x, size_y);

                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_yz;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (iy < end_y && iz < end_z) {
                    j = sub2ind_3D(ix, iy+1, iz+1, size_x, size_y);

                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_yz;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (ix > 0 && iz > 0) {
                    j = sub2ind_3D(ix-1, iy, iz-1, size_x, size_y);

                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_xz;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (ix < end_x && iz > 0) {
                    j = sub2ind_3D(ix+1, iy, iz-1, size_x, size_y);

                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_xz;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (ix > 0 && iz < end_z) {
                    j = sub2ind_3D(ix-1, iy, iz+1, size_x, size_y);

                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_xz;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (ix < end_x && iz < end_z) {
                    j = sub2ind_3D(ix+1, iy, iz+1, size_x, size_y);

                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_xz;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }

                // --------------------------------------------------------
                // 3-jump neighbours
                // --------------------------------------------------------
                if (ix > 0 && iy > 0 && iz > 0) {
                    j = sub2ind_3D(ix-1, iy-1, iz-1, size_x, size_y);

                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_xyz;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (ix > 0 && iy > 0 && iz < end_z) {
                    j = sub2ind_3D(ix-1, iy-1, iz+1, size_x, size_y);

                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_xyz;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (ix > 0 && iy < end_y && iz > 0) {
                    j = sub2ind_3D(ix-1, iy+1, iz-1, size_x, size_y);

                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_xyz;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (ix < end_x && iy > 0 && iz > 0) {
                    j = sub2ind_3D(ix+1, iy-1, iz-1, size_x, size_y);

                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_xyz;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (ix > 0 && iy < end_y && iz < end_z) {
                    j = sub2ind_3D(ix-1, iy+1, iz+1, size_x, size_y);

                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_xyz;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (ix < end_x && iy > 0 && iz < end_z) {
                    j = sub2ind_3D(ix+1, iy-1, iz+1, size_x, size_y);

                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_xyz;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (ix < end_x && iy < end_y && iz > 0) {
                    j = sub2ind_3D(ix+1, iy+1, iz-1, size_x, size_y);

                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_xyz;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (ix < end_x && iy < end_y && iz < end_z) {
                    j = sub2ind_3D(ix+1, iy+1, iz+1, size_x, size_y);

                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_xyz;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
            }
        }
        grow_step += 1;
    }

    // save_output_nifti(fout, "point1_distance", flood_dist, false);

    // Find 2/3 of the max distance
    float max_distance = 0;
    for (uint32_t ii = 0; ii != nr_voi; ++ii) {
        i = *(voi_id + ii);
        if (*(flood_dist_data + i) > max_distance) {
            max_distance = *(flood_dist_data + i);
        }
    }
    max_distance *= 2;
    max_distance /= 3;

    // Translate perimeter distances to find the next zero crossing point
    for (uint32_t ii = 0; ii != nr_voi; ++ii) {
        i = *(voi_id + ii);
        if (*(perimeter_data + i) == 1) {
            d = *(flood_dist_data + i);
            d -= max_distance;
            *(flood_dist_data + i) = abs(d);
        }
    }

    // NOTE(Faruk): Other 2 points are the ones closest to the 2/3 of the max
    // distance from point 1.
    int idx_point2;
    float point2_dist = std::numeric_limits<float>::infinity();
    for (uint32_t ii = 0; ii != nr_voi; ++ii) {
        i = *(voi_id + ii);
        if (*(perimeter_data + i) == 1) {
            d = *(flood_dist_data + i);
            if (d < point2_dist) {
                point2_dist = d;
                idx_point2 = i;
            }
        }
    }
    *(triplet_data + idx_point2) = 2;

    // ========================================================================
    // From point 1 & 2, find point 3 as the fastest away voxel from both
    // ========================================================================
    // Initialize grow volume
    for (uint32_t i = 0; i != nr_voxels; ++i) {
        *(flood_step_data + i) = 0.;
        *(flood_dist_data + i) = 0.;
    }
    *(flood_step_data + idx_point1) = 1.;
    *(flood_step_data + idx_point2) = 1.;

    // Reset some parameters
    flood_dist_thr = std::numeric_limits<float>::infinity();
    grow_step = 1;
    voxel_counter = nr_voxels;

    while (voxel_counter != 0) {
        voxel_counter = 0;
        for (uint32_t ii = 0; ii != nr_voi; ++ii) {
            i = *(voi_id + ii);  // Map subset to full set
            if (*(flood_step_data + i) == grow_step) {
                tie(ix, iy, iz) = ind2sub_3D(i, size_x, size_y);
                voxel_counter += 1;

                // --------------------------------------------------------
                // 1-jump neighbours
                // --------------------------------------------------------
                if (ix > 0) {
                    j = sub2ind_3D(ix-1, iy, iz, size_x, size_y);
                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dX;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (ix < end_x) {
                    j = sub2ind_3D(ix+1, iy, iz, size_x, size_y);
                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dX;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (iy > 0) {
                    j = sub2ind_3D(ix, iy-1, iz, size_x, size_y);
                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dY;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (iy < end_y) {
                    j = sub2ind_3D(ix, iy+1, iz, size_x, size_y);
                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dY;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (iz > 0) {
                    j = sub2ind_3D(ix, iy, iz-1, size_x, size_y);
                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dZ;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (iz < end_z) {
                    j = sub2ind_3D(ix, iy, iz+1, size_x, size_y);

                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dZ;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                // --------------------------------------------------------
                // 2-jump neighbours
                // --------------------------------------------------------
                if (ix > 0 && iy > 0) {
                    j = sub2ind_3D(ix-1, iy-1, iz, size_x, size_y);

                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_xy;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (ix > 0 && iy < end_y) {
                    j = sub2ind_3D(ix-1, iy+1, iz, size_x, size_y);

                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_xy;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (ix < end_x && iy > 0) {
                    j = sub2ind_3D(ix+1, iy-1, iz, size_x, size_y);

                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_xy;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (ix < end_x && iy < end_y) {
                    j = sub2ind_3D(ix+1, iy+1, iz, size_x, size_y);

                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_xy;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (iy > 0 && iz > 0) {
                    j = sub2ind_3D(ix, iy-1, iz-1, size_x, size_y);

                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_yz;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (iy > 0 && iz < end_z) {
                    j = sub2ind_3D(ix, iy-1, iz+1, size_x, size_y);

                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_yz;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (iy < end_y && iz > 0) {
                    j = sub2ind_3D(ix, iy+1, iz-1, size_x, size_y);

                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_yz;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (iy < end_y && iz < end_z) {
                    j = sub2ind_3D(ix, iy+1, iz+1, size_x, size_y);

                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_yz;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (ix > 0 && iz > 0) {
                    j = sub2ind_3D(ix-1, iy, iz-1, size_x, size_y);

                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_xz;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (ix < end_x && iz > 0) {
                    j = sub2ind_3D(ix+1, iy, iz-1, size_x, size_y);

                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_xz;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (ix > 0 && iz < end_z) {
                    j = sub2ind_3D(ix-1, iy, iz+1, size_x, size_y);

                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_xz;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (ix < end_x && iz < end_z) {
                    j = sub2ind_3D(ix+1, iy, iz+1, size_x, size_y);

                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_xz;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }

                // --------------------------------------------------------
                // 3-jump neighbours
                // --------------------------------------------------------
                if (ix > 0 && iy > 0 && iz > 0) {
                    j = sub2ind_3D(ix-1, iy-1, iz-1, size_x, size_y);

                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_xyz;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (ix > 0 && iy > 0 && iz < end_z) {
                    j = sub2ind_3D(ix-1, iy-1, iz+1, size_x, size_y);

                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_xyz;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (ix > 0 && iy < end_y && iz > 0) {
                    j = sub2ind_3D(ix-1, iy+1, iz-1, size_x, size_y);

                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_xyz;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (ix < end_x && iy > 0 && iz > 0) {
                    j = sub2ind_3D(ix+1, iy-1, iz-1, size_x, size_y);

                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_xyz;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (ix > 0 && iy < end_y && iz < end_z) {
                    j = sub2ind_3D(ix-1, iy+1, iz+1, size_x, size_y);

                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_xyz;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (ix < end_x && iy > 0 && iz < end_z) {
                    j = sub2ind_3D(ix+1, iy-1, iz+1, size_x, size_y);

                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_xyz;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (ix < end_x && iy < end_y && iz > 0) {
                    j = sub2ind_3D(ix+1, iy+1, iz-1, size_x, size_y);

                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_xyz;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
                if (ix < end_x && iy < end_y && iz < end_z) {
                    j = sub2ind_3D(ix+1, iy+1, iz+1, size_x, size_y);

                    if (*(perimeter_data + j) == 1) {
                        d = *(flood_dist_data + i) + dia_xyz;
                        if (d < *(flood_dist_data + j)
                            || *(flood_dist_data + j) == 0) {
                            *(flood_dist_data + j) = d;
                            *(flood_step_data + j) = grow_step + 1;
                        }
                    }
                }
            }
        }
        grow_step += 1;
    }

    // Find max distance from both points 1 & 2
    max_distance = 0;
    int idx_point3;
    for (uint32_t ii = 0; ii != nr_voi; ++ii) {
        i = *(voi_id + ii);
        if (*(flood_dist_data + i) > max_distance) {
            max_distance = *(flood_dist_data + i);
            idx_point3 = i;
        }
    }
    *(triplet_data + idx_point3) = 3;

    save_output_nifti(fout, "triplet", triplet, false);

    // ========================================================================
    // Flooding distance from each triplet as output
    // ========================================================================
    for (uint32_t p = 1; p < 4; ++p) {
        // Initialize grow volume
        for (uint32_t i = 0; i != nr_voxels; ++i) {
            *(flood_step_data + i) = 0.;
            *(flood_dist_data + i) = 0.;
        }

        // NOTE(Faruk): Might handle this with vectors in the future.
        if (p == 1) {
            *(flood_step_data + idx_point1) = 1.;
        } else if (p == 2) {
            *(flood_step_data + idx_point2) = 1.;
        } else if (p == 3) {
            *(flood_step_data + idx_point3) = 1.;
        }

        // Reset some parameters
        flood_dist_thr = std::numeric_limits<float>::infinity();
        grow_step = 1;
        voxel_counter = nr_voxels;

        while (voxel_counter != 0) {
            voxel_counter = 0;
            for (uint32_t ii = 0; ii != nr_voi; ++ii) {
                i = *(voi_id + ii);  // Map subset to full set
                if (*(flood_step_data + i) == grow_step) {
                    tie(ix, iy, iz) = ind2sub_3D(i, size_x, size_y);
                    voxel_counter += 1;

                    // --------------------------------------------------------
                    // 1-jump neighbours
                    // --------------------------------------------------------
                    if (ix > 0) {
                        j = sub2ind_3D(ix-1, iy, iz, size_x, size_y);
                        if (*(nii_midgm_data + j) == 1) {
                            d = *(flood_dist_data + i) + dX;
                            if (d < *(flood_dist_data + j)
                                || *(flood_dist_data + j) == 0) {
                                *(flood_dist_data + j) = d;
                                *(flood_step_data + j) = grow_step + 1;
                            }
                        }
                    }
                    if (ix < end_x) {
                        j = sub2ind_3D(ix+1, iy, iz, size_x, size_y);
                        if (*(nii_midgm_data + j) == 1) {
                            d = *(flood_dist_data + i) + dX;
                            if (d < *(flood_dist_data + j)
                                || *(flood_dist_data + j) == 0) {
                                *(flood_dist_data + j) = d;
                                *(flood_step_data + j) = grow_step + 1;
                            }
                        }
                    }
                    if (iy > 0) {
                        j = sub2ind_3D(ix, iy-1, iz, size_x, size_y);
                        if (*(nii_midgm_data + j) == 1) {
                            d = *(flood_dist_data + i) + dY;
                            if (d < *(flood_dist_data + j)
                                || *(flood_dist_data + j) == 0) {
                                *(flood_dist_data + j) = d;
                                *(flood_step_data + j) = grow_step + 1;
                            }
                        }
                    }
                    if (iy < end_y) {
                        j = sub2ind_3D(ix, iy+1, iz, size_x, size_y);
                        if (*(nii_midgm_data + j) == 1) {
                            d = *(flood_dist_data + i) + dY;
                            if (d < *(flood_dist_data + j)
                                || *(flood_dist_data + j) == 0) {
                                *(flood_dist_data + j) = d;
                                *(flood_step_data + j) = grow_step + 1;
                            }
                        }
                    }
                    if (iz > 0) {
                        j = sub2ind_3D(ix, iy, iz-1, size_x, size_y);
                        if (*(nii_midgm_data + j) == 1) {
                            d = *(flood_dist_data + i) + dZ;
                            if (d < *(flood_dist_data + j)
                                || *(flood_dist_data + j) == 0) {
                                *(flood_dist_data + j) = d;
                                *(flood_step_data + j) = grow_step + 1;
                            }
                        }
                    }
                    if (iz < end_z) {
                        j = sub2ind_3D(ix, iy, iz+1, size_x, size_y);

                        if (*(nii_midgm_data + j) == 1) {
                            d = *(flood_dist_data + i) + dZ;
                            if (d < *(flood_dist_data + j)
                                || *(flood_dist_data + j) == 0) {
                                *(flood_dist_data + j) = d;
                                *(flood_step_data + j) = grow_step + 1;
                            }
                        }
                    }
                    // --------------------------------------------------------
                    // 2-jump neighbours
                    // --------------------------------------------------------
                    if (ix > 0 && iy > 0) {
                        j = sub2ind_3D(ix-1, iy-1, iz, size_x, size_y);

                        if (*(nii_midgm_data + j) == 1) {
                            d = *(flood_dist_data + i) + dia_xy;
                            if (d < *(flood_dist_data + j)
                                || *(flood_dist_data + j) == 0) {
                                *(flood_dist_data + j) = d;
                                *(flood_step_data + j) = grow_step + 1;
                            }
                        }
                    }
                    if (ix > 0 && iy < end_y) {
                        j = sub2ind_3D(ix-1, iy+1, iz, size_x, size_y);

                        if (*(nii_midgm_data + j) == 1) {
                            d = *(flood_dist_data + i) + dia_xy;
                            if (d < *(flood_dist_data + j)
                                || *(flood_dist_data + j) == 0) {
                                *(flood_dist_data + j) = d;
                                *(flood_step_data + j) = grow_step + 1;
                            }
                        }
                    }
                    if (ix < end_x && iy > 0) {
                        j = sub2ind_3D(ix+1, iy-1, iz, size_x, size_y);

                        if (*(nii_midgm_data + j) == 1) {
                            d = *(flood_dist_data + i) + dia_xy;
                            if (d < *(flood_dist_data + j)
                                || *(flood_dist_data + j) == 0) {
                                *(flood_dist_data + j) = d;
                                *(flood_step_data + j) = grow_step + 1;
                            }
                        }
                    }
                    if (ix < end_x && iy < end_y) {
                        j = sub2ind_3D(ix+1, iy+1, iz, size_x, size_y);

                        if (*(nii_midgm_data + j) == 1) {
                            d = *(flood_dist_data + i) + dia_xy;
                            if (d < *(flood_dist_data + j)
                                || *(flood_dist_data + j) == 0) {
                                *(flood_dist_data + j) = d;
                                *(flood_step_data + j) = grow_step + 1;
                            }
                        }
                    }
                    if (iy > 0 && iz > 0) {
                        j = sub2ind_3D(ix, iy-1, iz-1, size_x, size_y);

                        if (*(nii_midgm_data + j) == 1) {
                            d = *(flood_dist_data + i) + dia_yz;
                            if (d < *(flood_dist_data + j)
                                || *(flood_dist_data + j) == 0) {
                                *(flood_dist_data + j) = d;
                                *(flood_step_data + j) = grow_step + 1;
                            }
                        }
                    }
                    if (iy > 0 && iz < end_z) {
                        j = sub2ind_3D(ix, iy-1, iz+1, size_x, size_y);

                        if (*(nii_midgm_data + j) == 1) {
                            d = *(flood_dist_data + i) + dia_yz;
                            if (d < *(flood_dist_data + j)
                                || *(flood_dist_data + j) == 0) {
                                *(flood_dist_data + j) = d;
                                *(flood_step_data + j) = grow_step + 1;
                            }
                        }
                    }
                    if (iy < end_y && iz > 0) {
                        j = sub2ind_3D(ix, iy+1, iz-1, size_x, size_y);

                        if (*(nii_midgm_data + j) == 1) {
                            d = *(flood_dist_data + i) + dia_yz;
                            if (d < *(flood_dist_data + j)
                                || *(flood_dist_data + j) == 0) {
                                *(flood_dist_data + j) = d;
                                *(flood_step_data + j) = grow_step + 1;
                            }
                        }
                    }
                    if (iy < end_y && iz < end_z) {
                        j = sub2ind_3D(ix, iy+1, iz+1, size_x, size_y);

                        if (*(nii_midgm_data + j) == 1) {
                            d = *(flood_dist_data + i) + dia_yz;
                            if (d < *(flood_dist_data + j)
                                || *(flood_dist_data + j) == 0) {
                                *(flood_dist_data + j) = d;
                                *(flood_step_data + j) = grow_step + 1;
                            }
                        }
                    }
                    if (ix > 0 && iz > 0) {
                        j = sub2ind_3D(ix-1, iy, iz-1, size_x, size_y);

                        if (*(nii_midgm_data + j) == 1) {
                            d = *(flood_dist_data + i) + dia_xz;
                            if (d < *(flood_dist_data + j)
                                || *(flood_dist_data + j) == 0) {
                                *(flood_dist_data + j) = d;
                                *(flood_step_data + j) = grow_step + 1;
                            }
                        }
                    }
                    if (ix < end_x && iz > 0) {
                        j = sub2ind_3D(ix+1, iy, iz-1, size_x, size_y);

                        if (*(nii_midgm_data + j) == 1) {
                            d = *(flood_dist_data + i) + dia_xz;
                            if (d < *(flood_dist_data + j)
                                || *(flood_dist_data + j) == 0) {
                                *(flood_dist_data + j) = d;
                                *(flood_step_data + j) = grow_step + 1;
                            }
                        }
                    }
                    if (ix > 0 && iz < end_z) {
                        j = sub2ind_3D(ix-1, iy, iz+1, size_x, size_y);

                        if (*(nii_midgm_data + j) == 1) {
                            d = *(flood_dist_data + i) + dia_xz;
                            if (d < *(flood_dist_data + j)
                                || *(flood_dist_data + j) == 0) {
                                *(flood_dist_data + j) = d;
                                *(flood_step_data + j) = grow_step + 1;
                            }
                        }
                    }
                    if (ix < end_x && iz < end_z) {
                        j = sub2ind_3D(ix+1, iy, iz+1, size_x, size_y);

                        if (*(nii_midgm_data + j) == 1) {
                            d = *(flood_dist_data + i) + dia_xz;
                            if (d < *(flood_dist_data + j)
                                || *(flood_dist_data + j) == 0) {
                                *(flood_dist_data + j) = d;
                                *(flood_step_data + j) = grow_step + 1;
                            }
                        }
                    }

                    // --------------------------------------------------------
                    // 3-jump neighbours
                    // --------------------------------------------------------
                    if (ix > 0 && iy > 0 && iz > 0) {
                        j = sub2ind_3D(ix-1, iy-1, iz-1, size_x, size_y);

                        if (*(nii_midgm_data + j) == 1) {
                            d = *(flood_dist_data + i) + dia_xyz;
                            if (d < *(flood_dist_data + j)
                                || *(flood_dist_data + j) == 0) {
                                *(flood_dist_data + j) = d;
                                *(flood_step_data + j) = grow_step + 1;
                            }
                        }
                    }
                    if (ix > 0 && iy > 0 && iz < end_z) {
                        j = sub2ind_3D(ix-1, iy-1, iz+1, size_x, size_y);

                        if (*(nii_midgm_data + j) == 1) {
                            d = *(flood_dist_data + i) + dia_xyz;
                            if (d < *(flood_dist_data + j)
                                || *(flood_dist_data + j) == 0) {
                                *(flood_dist_data + j) = d;
                                *(flood_step_data + j) = grow_step + 1;
                            }
                        }
                    }
                    if (ix > 0 && iy < end_y && iz > 0) {
                        j = sub2ind_3D(ix-1, iy+1, iz-1, size_x, size_y);

                        if (*(nii_midgm_data + j) == 1) {
                            d = *(flood_dist_data + i) + dia_xyz;
                            if (d < *(flood_dist_data + j)
                                || *(flood_dist_data + j) == 0) {
                                *(flood_dist_data + j) = d;
                                *(flood_step_data + j) = grow_step + 1;
                            }
                        }
                    }
                    if (ix < end_x && iy > 0 && iz > 0) {
                        j = sub2ind_3D(ix+1, iy-1, iz-1, size_x, size_y);

                        if (*(nii_midgm_data + j) == 1) {
                            d = *(flood_dist_data + i) + dia_xyz;
                            if (d < *(flood_dist_data + j)
                                || *(flood_dist_data + j) == 0) {
                                *(flood_dist_data + j) = d;
                                *(flood_step_data + j) = grow_step + 1;
                            }
                        }
                    }
                    if (ix > 0 && iy < end_y && iz < end_z) {
                        j = sub2ind_3D(ix-1, iy+1, iz+1, size_x, size_y);

                        if (*(nii_midgm_data + j) == 1) {
                            d = *(flood_dist_data + i) + dia_xyz;
                            if (d < *(flood_dist_data + j)
                                || *(flood_dist_data + j) == 0) {
                                *(flood_dist_data + j) = d;
                                *(flood_step_data + j) = grow_step + 1;
                            }
                        }
                    }
                    if (ix < end_x && iy > 0 && iz < end_z) {
                        j = sub2ind_3D(ix+1, iy-1, iz+1, size_x, size_y);

                        if (*(nii_midgm_data + j) == 1) {
                            d = *(flood_dist_data + i) + dia_xyz;
                            if (d < *(flood_dist_data + j)
                                || *(flood_dist_data + j) == 0) {
                                *(flood_dist_data + j) = d;
                                *(flood_step_data + j) = grow_step + 1;
                            }
                        }
                    }
                    if (ix < end_x && iy < end_y && iz > 0) {
                        j = sub2ind_3D(ix+1, iy+1, iz-1, size_x, size_y);

                        if (*(nii_midgm_data + j) == 1) {
                            d = *(flood_dist_data + i) + dia_xyz;
                            if (d < *(flood_dist_data + j)
                                || *(flood_dist_data + j) == 0) {
                                *(flood_dist_data + j) = d;
                                *(flood_step_data + j) = grow_step + 1;
                            }
                        }
                    }
                    if (ix < end_x && iy < end_y && iz < end_z) {
                        j = sub2ind_3D(ix+1, iy+1, iz+1, size_x, size_y);

                        if (*(nii_midgm_data + j) == 1) {
                            d = *(flood_dist_data + i) + dia_xyz;
                            if (d < *(flood_dist_data + j)
                                || *(flood_dist_data + j) == 0) {
                                *(flood_dist_data + j) = d;
                                *(flood_step_data + j) = grow_step + 1;
                            }
                        }
                    }
                }
            }
            grow_step += 1;
        }
    save_output_nifti(fout, "point" + std::to_string(p) + "_dist", flood_dist, true);
    }
    cout << "\n  Finished." << endl;
    return 0;
}