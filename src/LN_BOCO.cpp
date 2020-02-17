
#include "./common.h"
#include "./renzo_stat.h"
#include "./utils.h"

int show_help(void) {
    printf(
    "LN_BOCO: This program does BOLD correction in SS-SI VASO. It does \n"
    "         the division of nulled and not nulled imaged. \n"
    "\n"
    "Usage:\n"
    "    LN_BOCO -Nulled Nulled_intemp.nii -BOLD BOLD_intemp.nii \n"
    "    LN_BOCO -Nulled Nulled_intemp.nii -BOLD BOLD_intemp.nii -shift \n"
    "    LN_BOCO -Nulled Nulled_intemp.nii -BOLD BOLD_intemp.nii -trialBOCO 24 \n"
    "\n"
    "Options:\n"
    "    -help      : Show this help.\n"
    "    -Nulled    : Nulled (VASO) time series that needs to be BOLD \n"
    "               : corrected.\n"
    "    -BOLD      : Reference BOLD time series without a VASO contrast.\n"
    "    -shift     : (Optional) Estimate the correlation of BOLD and VASO \n"
    "                 for temporal shifts.\n"
    "    -trialBOCO : First average trials and then do the BOLD correction. \n"
    "                 The parameter is the trial duration in TRs.\n"
    "\n"
    "Notes:\n"
    "    - Here it is assumed that BOLD and VASO refer to the double TR: \n"
    "        3dUpsample -overwrite -datum short -prefix Nulled_intemp.nii -n 2 -input Nulled.nii \n"
    "        3dUpsample -overwrite -datum short -prefix BOLD_intemp.nii -n 2 -input BOLD.nii \n"
    "    - Here I assume that they have the same spatiotemporal dimensions. \n"
    "\n");
    return 0;
}

int main(int argc, char * argv[]) {
    // nifti_image* nim_input=NULL;
    char * fin_1 = NULL, * fin_2 = NULL;
    int ac, disp_float_eg = 0, shift = 0;
    int trialdur = 0;
    if (argc < 2) {  // Typing '-help' is sooo much work
        return show_help();
    }

    // Process user options: 4 are valid presently
    for (ac = 1; ac < argc; ac++) {
        if (!strncmp(argv[ac], "-h", 2)) {
            return show_help();
        } else if (!strcmp(argv[ac], "-Nulled")) {
            if (++ac >= argc) {
                fprintf(stderr, "** missing argument for -Nulled\n");
                return 1;
            }
            fin_1 = argv[ac];
        } else if (!strcmp(argv[ac], "-BOLD")) {
            if (++ac >= argc) {
                fprintf(stderr, "** missing argument for -BOLD\n");
                return 1;
            }
            fin_2 = argv[ac];
        } else if (!strcmp(argv[ac], "-trialBOCO")) {
            if (++ac >= argc) {
                fprintf(stderr, "** missing argument for -trialBOCO\n");
                return 1;
            }
            trialdur = atof(argv[ac]);
        } else if (!strcmp(argv[ac], "-shift")) {
            shift = 1;
            cout << "Do a correlation analysis with temporal shifts."  << endl;
        } else {
            fprintf(stderr, "** invalid option, '%s'\n", argv[ac]);
            return 1;
        }
    }

    if (!fin_1) {
        fprintf(stderr, "** missing option '-Nulled'.\n");
        return 1;
    }
    if (!fin_2) {
        fprintf(stderr, "** missing option '-BOLD'.\n");
        return 1;
    }

    // Read input dataset, including data
    nifti_image* nii1 = nifti_image_read(fin_1, 1);
    if (!nii1) {
        fprintf(stderr, "** failed to read NIfTI from '%s'.\n", fin_1);
        return 2;
    }

    nifti_image* nii2 = nifti_image_read(fin_2, 1);
    if (!nii2) {
        fprintf(stderr, "** failed to read NIfTI from '%s'.\n", fin_2);
        return 2;
    }

    log_welcome("LN_BOCO");
    log_nifti_descriptives(nii1);
    log_nifti_descriptives(nii2);

    // Get dimensions of input
    const int size_x = nii1->nx;  // phase
    const int size_y = nii1->ny;  // read
    const int size_z = nii1->nz;  // slice
    const int size_t = nii1->nt;  // time
    const int nx = nii1->nx;
    const int nxy = nii1->nx * nii1->ny;
    const int nxyz = nii1->nx * nii1->ny * nii1->nz;

    //////////////////////////////////////////////////////////////
    // Fixing potential problems with different input datatypes //
    // here, I am loading them in their native datatype         //
    // and translate them to the datatime I like best           //
    //////////////////////////////////////////////////////////////

    nifti_image* nii1_temp = nifti_copy_nim_info(nii1);
    nii1_temp->datatype = NIFTI_TYPE_FLOAT32;
    nii1_temp->nbyper = sizeof(float);
    nii1_temp->data = calloc(nii1_temp->nvox, nii1_temp->nbyper);
    float* nii1_temp_data = static_cast<float*>(nii1_temp->data);

    if (nii1->datatype == NIFTI_TYPE_INT16) {
        int16_t* nim_file_1i_data = static_cast<int16_t*>(nii1->data);
        FOR_EACH_VOXEL_TZYX
            *(nii1_temp_data + VOXEL_ID) =
                static_cast<float>(*(nim_file_1i_data + VOXEL_ID));
        END_FOR_EACH_VOXEL_TZYX
    }
    if (nii1->datatype == NIFTI_TYPE_FLOAT32) {
        float* nim_file_1i_data = static_cast<float*>(nii1->data);
        FOR_EACH_VOXEL_TZYX
            *(nii1_temp_data + VOXEL_ID) =
                static_cast<float>(*(nim_file_1i_data + VOXEL_ID));
        END_FOR_EACH_VOXEL_TZYX
    }


    nifti_image* nii2_temp = nifti_copy_nim_info(nii1);
    nii2_temp->datatype = NIFTI_TYPE_FLOAT32;
    nii2_temp->nbyper = sizeof(float);
    nii2_temp->data = calloc(nii2_temp->nvox, nii2_temp->nbyper);
    float* nii2_data = static_cast<float*>(nii2_temp->data);

    if (nii2->datatype == NIFTI_TYPE_INT16) {
        int16_t* nii2_temp_data = static_cast<int16_t*>(nii2->data);
        FOR_EACH_VOXEL_TZYX
            *(nii2_data + VOXEL_ID) =
                static_cast<float>(*(nii2_temp_data + VOXEL_ID));
        END_FOR_EACH_VOXEL_TZYX
    }
    if (nii2->datatype == NIFTI_TYPE_FLOAT32) {
        float* nii2_temp_data = static_cast<float*>(nii2->data);
        FOR_EACH_VOXEL_TZYX
            *(nii2_data + VOXEL_ID) =
                static_cast<float>(*(nii2_temp_data + VOXEL_ID));
        END_FOR_EACH_VOXEL_TZYX
    }

    // ========================================================================

    // float current_vaso = 0;
    nifti_image* boco_vaso = nifti_copy_nim_info(nii1_temp);
    boco_vaso->datatype = NIFTI_TYPE_FLOAT32;
    boco_vaso->nbyper = sizeof(float);
    boco_vaso->data = calloc(boco_vaso->nvox, boco_vaso->nbyper);
    float  *boco_vaso_data = static_cast<float*>(boco_vaso->data);

    // AVERAGE across Trials
    for (int iz = 0; iz < size_z; ++iz) {
        for (int iy = 0; iy < size_y; ++iy) {
            for (int ix = 0; ix < size_x; ++ix) {
                for (int it = 0; it < size_t; ++it) {
                    *(boco_vaso_data + VOXEL_ID) = *(nii1_temp_data + VOXEL_ID)
                                                   / (*(nii2_data + VOXEL_ID));
                }
            }
        }
    }
    // Clean VASO values that are unrealistic
    for (int iz = 0; iz < size_z; ++iz) {
        for (int iy = 0; iy < size_y; ++iy) {
            for (int ix = 0; ix < size_x; ++ix) {
                for (int it = 0; it < size_t; ++it) {
                    if (*(boco_vaso_data + VOXEL_ID) <= 0) {
                        *(boco_vaso_data + VOXEL_ID) = 0;
                    }
                    if (*(boco_vaso_data + VOXEL_ID) >= 5) {
                        *(boco_vaso_data + VOXEL_ID) = 5;
                    }
                }
            }
        }
    }
    if (shift == 1) {
        nifti_image* correl_file  = nifti_copy_nim_info(nii1_temp);
        correl_file->nt = 7;
        correl_file->nvox = nii1_temp->nvox / size_t *7;
        correl_file->datatype = NIFTI_TYPE_FLOAT32;
        correl_file->nbyper = sizeof(float);
        correl_file->data = calloc(correl_file->nvox, correl_file->nbyper);
        float* correl_file_data = static_cast<float*>(correl_file->data);

        double vec_file1[size_t];
        double vec_file2[size_t];

        for (int shift = -3; shift <= 3; ++shift) {
            cout << "  Calculating shift = " << shift << endl;
            for (int iz = 0; iz < size_z; ++iz) {
                for (int iy = 0; iy < size_y; ++iy) {
                    for (int ix = 0; ix < size_x; ++ix) {
                        for (int it = 3; it < size_t-3; ++it) {
                            *(boco_vaso_data + VOXEL_ID) =
                                *(nii1_temp_data + VOXEL_ID)
                                / (*(nii2_data + nxyz * (it + shift)
                                     + nxy * iz + nx * iy + nx));
                        }
                        for (int it = 0; it < size_t; ++it) {
                            vec_file1[it] = *(boco_vaso_data + VOXEL_ID);
                            vec_file2[it] = *(nii2_data + VOXEL_ID);
                        }
                        *(correl_file_data + nxyz * (shift+3) + nxy * iz + nx * iy + nx) = ren_correl(vec_file1, vec_file2, size_t);
                    }
                }
            }
        }
        // Get back to default
        for (int iz = 0; iz < size_z; ++iz) {
            for (int iy = 0; iy < size_y; ++iy) {
                for (int ix = 0; ix < size_x; ++ix) {
                    for (int it = 0; it < size_t; ++it) {
                        *(boco_vaso_data + VOXEL_ID) =
                            *(nii1_temp_data + VOXEL_ID)
                            / (*(nii2_data + VOXEL_ID));
                    }
                }
            }
        }
        // Clean VASO values that are unrealistic
        for (int iz = 0; iz < size_z; ++iz) {
            for (int iy = 0; iy < size_y; ++iy) {
                for (int ix = 0; ix < size_x; ++ix) {
                    for (int it = 0; it < size_t; ++it) {
                        if (*(boco_vaso_data + VOXEL_ID) <= 0) {
                            *(boco_vaso_data + VOXEL_ID) = 0;
                        }
                        if (*(boco_vaso_data + VOXEL_ID) >= 2) {
                            *(boco_vaso_data + VOXEL_ID) = 2;
                        }
                    }
                }
            }
        }

        string prefix = "correlated_";
        string filename_1 = (string) (fin_1);
        string outfilename = prefix+filename_1;
        log_output(outfilename.c_str());
        // cout << "  Writing as = " << outfilename.c_str() << endl;

        const char* fout_1 = outfilename.c_str();
        if (nifti_set_filenames(correl_file, fout_1, 1, 1)) {
            return 1;
        }
        nifti_image_write(correl_file);
    }  // shift loop closed

    if (trialdur != 0) {
        cout << "  Also do BOLD correction after trial average." << endl;
        cout << "  Trial duration is " << trialdur << ". This means there are "
             << static_cast<float>(size_t)/static_cast<float>(trialdur)
             << " trials recorded here." << endl;

        int numberofTrials = size_t/trialdur;
        // Trial average file
        nifti_image* triav_file = nifti_copy_nim_info(nii1_temp);
        triav_file->nt = trialdur;
        triav_file->nvox = nii1_temp->nvox / size_t * trialdur;
        triav_file->datatype = NIFTI_TYPE_FLOAT32;
        triav_file->nbyper = sizeof(float);
        triav_file->data = calloc(triav_file->nvox, triav_file->nbyper);
        float* triav_file_data = static_cast<float*>(triav_file->data);

        nifti_image* triav_B_file = nifti_copy_nim_info(nii1_temp);
        triav_B_file->nt = trialdur;
        triav_B_file->nvox = nii1_temp->nvox / size_t * trialdur;
        triav_B_file->datatype = NIFTI_TYPE_FLOAT32;
        triav_B_file->nbyper = sizeof(float);
        triav_B_file->data = calloc(triav_B_file->nvox, triav_B_file->nbyper);
        float* triav_B_file_data = static_cast<float*>(triav_B_file->data);

        float AV_Nulled[trialdur];
        float AV_BOLD[trialdur];

        for (int iz = 0; iz < size_z; ++iz) {
            for (int iy = 0; iy < size_y; ++iy) {
                for (int ix = 0; ix < size_x; ++ix) {
                    for (int it = 0; it < trialdur; ++it) {
                        AV_Nulled[it] = 0;
                        AV_BOLD[it] = 0;
                    }
                    for (int it = 0; it < trialdur * numberofTrials; ++it) {
                        AV_Nulled[it%trialdur] = AV_Nulled[it%trialdur]
                                                 + (*(nii1_temp_data + VOXEL_ID)) / numberofTrials;
                        AV_BOLD[it%trialdur] = AV_BOLD[it%trialdur]
                                               + (*(nii2_data + VOXEL_ID)) / numberofTrials;
                    }
                    for (int it = 0; it < trialdur; ++it) {
                        *(triav_file_data + VOXEL_ID) = AV_Nulled[it] / AV_BOLD[it];
                        *(triav_B_file_data + VOXEL_ID) = AV_BOLD[it];
                    }
                }
            }
        }

        // Clean VASO values that are unrealistic
        for (int iz = 0; iz < size_z; ++iz) {
            for (int iy = 0; iy < size_y; ++iy) {
                for (int ix = 0; ix < size_x; ++ix) {
                    for (int it = 0; it < trialdur; ++it) {
                        if (*(triav_file_data + VOXEL_ID) <= 0) {
                            *(triav_file_data + VOXEL_ID) = 0;
                        }
                        if (*(triav_file_data + VOXEL_ID) >= 2) {
                            *(triav_file_data + VOXEL_ID) = 2;
                        }
                    }
                }
            }
        }

        const char* fout_trial = "VASO_trialAV_LN.nii";
        if (nifti_set_filenames(triav_file, fout_trial, 1, 1)) {
            return 1;
        }
        nifti_image_write(triav_file);

        const char* fout_trial_BOLD = "BOLD_trialAV_LN.nii";
        if (nifti_set_filenames(triav_B_file, fout_trial_BOLD, 1, 1)) {
            return 1;
        }
        nifti_image_write(triav_B_file);
    }  // Trial Average loop closed

    // cout << "  Running also until here 5... " << endl;
    const char* fout_5 = "VASO_LN.nii";
    log_output(fout_5);
    if (nifti_set_filenames(boco_vaso, fout_5, 1, 1)) {
        return 1;
    }
    nifti_image_write(boco_vaso);

    // const char* fout_6 = "kootrGM.nii";
    // if (nifti_set_filenames(GMkoord2, fout_6 , 1, 1)) return 1;
    // nifti_image_write(GMkoord2);

    // koord.autowrite("koordinaten.nii", wopts, &prot);
    cout << "  Finished." << endl;
    return 0;
}
