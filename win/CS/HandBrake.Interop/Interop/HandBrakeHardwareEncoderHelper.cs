// --------------------------------------------------------------------------------------------------------------------
// <copyright file="HandBrakeHardwareEncoderHelper.cs" company="HandBrake Project (https://handbrake.fr)">
//   This file is part of the HandBrake source code - It may be used under the terms of the GNU General Public License.
// </copyright>
// <summary>
//   The System Information.
// </summary>
// --------------------------------------------------------------------------------------------------------------------

namespace HandBrake.Interop.Interop
{
    using System;
    using System.Runtime.InteropServices;

    using HandBrake.Interop.Interop.HbLib;

    public class HandBrakeHardwareEncoderHelper
    {
        private static bool? isNvencH264Available; // Local cache to prevent log spam.
        private static bool? isNvencH265Available;
        private static bool? isNVDecAvailable;

        private static bool? isQsvAvailable;

        private static bool? isDirectXAvailable;

        private static bool? isSafeMode;

        public static bool IsSafeMode
        {
            get
            {
                try
                {
                    if (isSafeMode != null)
                    {
                        return isSafeMode.Value;
                    }

                    if (RuntimeInformation.ProcessArchitecture == Architecture.Arm64)
                    {
                        return false;
                    }

                    isSafeMode = (HBFunctions.hb_qsv_available() + HBFunctions.hb_vce_h264_available()
                                                              + HBFunctions.hb_nvenc_h264_available()) == -3;

                    return isSafeMode.Value;
                }
                catch (Exception)
                {
                    // Silent failure. Typically this means the dll hasn't been built with --enable-qsv
                    return false;
                }
            }
        }

        /* QuickSync Support */


        public static bool IsQsvAvailable
        {
            get
            {
                try
                {
                    if (isQsvAvailable != null)
                    {
                        return isQsvAvailable.Value;
                    }

                    isQsvAvailable = HBFunctions.hb_qsv_available() > 0;
                    
                    return isQsvAvailable.Value; 
                }
                catch (Exception)
                {
                    // Silent failure. Typically this means the dll hasn't been built with --enable-qsv
                    return false;
                }
            }
        }

        public static bool IsQsvHyperEncodeAvailable
        {
            get
            {
                try
                {
                    return IsQsvAvailable && HBFunctions.hb_qsv_hyper_encode_available(HBFunctions.hb_qsv_get_adapter_index()) > 0;
                }
                catch (Exception)
                {
                    // Silent failure. Typically this means the dll hasn't been built with --enable-qsv
                    return false;
                }
            }
        }

        public static bool IsQsvAvailableH264
        {
            get
            {
                try
                {
                    return (HBFunctions.hb_qsv_available() & NativeConstants.HB_VCODEC_FFMPEG_QSV_H264) > 0;
                }
                catch (Exception)
                {
                    // Silent failure. Typically this means the dll hasn't been built with --enable-qsv
                    return false;
                }
            }
        }

        public static bool IsQsvAvailableH265
        {
            get
            {
                try
                {
                    return (HBFunctions.hb_qsv_available() & NativeConstants.HB_VCODEC_FFMPEG_QSV_H265) > 0;
                }
                catch (Exception)
                {
                    // Silent failure. Typically this means the dll hasn't been built with --enable-qsv
                    return false;
                }
            }
        }

        public static bool IsQsvAvailableH26510bit
        {
            get
            {
                try
                {
                    return (HBFunctions.hb_qsv_available() & NativeConstants.HB_VCODEC_FFMPEG_QSV_H265_10BIT) > 0;
                }
                catch (Exception)
                {
                    // Silent failure. Typically this means the dll hasn't been built with --enable-qsv
                    return false;
                }
            }
        }
        
        public static bool IsQsvAvailableAV1
        {
            get
            {
                try
                {
                    return (HBFunctions.hb_qsv_available() & NativeConstants.HB_VCODEC_FFMPEG_QSV_AV1) > 0;
                }
                catch (Exception)
                {
                    // Silent failure. Typically this means the dll hasn't been built with --enable-qsv
                    return false;
                }
            }
        }
        
        public static bool IsQsvAvailableAV110bit
        {
            get
            {
                try
                {
                    return (HBFunctions.hb_qsv_available() & NativeConstants.HB_VCODEC_FFMPEG_QSV_AV1_10BIT) > 0;
                }
                catch (Exception)
                {
                    // Silent failure. Typically this means the dll hasn't been built with --enable-qsv
                    return false;
                }
            }
        }

        /* DirectX Support */

        public static bool IsDirectXAvailable
        {
            get
            {
                try
                {
                    if (isDirectXAvailable != null)
                    {
                        return isDirectXAvailable.Value;
                    }

                    isDirectXAvailable = HBFunctions.hb_directx_available() > 0;
                    
                    return isDirectXAvailable.Value; 
                }
                catch (Exception)
                {
                    return false;
                }
            }
        }
        
        /* AMD VCE Support */

        public static bool IsVceH264Available
        {
            get
            {
                try
                {
                    return HBFunctions.hb_vce_h264_available() > 0;
                }
                catch (Exception)
                {
                    // Silent failure. Typically this means the dll hasn't been built with --enable-qsv
                    return false;
                }
            }
        }

        public static bool IsVceH265Available
        {
            get
            {
                try
                {
                    return HBFunctions.hb_vce_h265_available() > 0;
                }
                catch (Exception)
                {
                    // Silent failure. Typically this means the dll hasn't been built with --enable-qsv
                    return false;
                }
            }
        }

        public static bool IsVceAV1Available
        {
            get
            {
                try
                {
                    return HBFunctions.hb_vce_av1_available() > 0;
                }
                catch (Exception)
                {
                    // Silent failure. Typically this means the dll hasn't been built with --enable-vce
                    return false;
                }
            }
        }

        public static bool IsAMFDecAvailable
        {
            get
            {
                try
                {
                    return HBFunctions.hb_check_amfdec_available() > 0;
                }
                catch (Exception)
                {
                    // Silent failure. Typically this means the dll hasn't been built with --enable-amfdec
                    return false;
                }
            }
        }

        /* Nvidia NVEnc Support */

        public static bool IsNVEncH264Available
        {
            get
            {
                try
                {
                    if (isNvencH264Available == null)
                    {
                        isNvencH264Available = HBFunctions.hb_nvenc_h264_available() > 0;
                    }
                    
                    return isNvencH264Available.Value;
                }
                catch (Exception)
                {
                    // Silent failure. Typically this means the dll hasn't been built with --enable-qsv
                    return false;
                }
            }
        }

        public static bool IsNVEncH265Available
        {
            get
            {
                try
                {
                    if (!IsNVEncH264Available)
                    {
                        return false;
                    }

                    if (isNvencH265Available == null)
                    {
                        isNvencH265Available = HBFunctions.hb_nvenc_h265_available() > 0;
                    }

                    return isNvencH265Available.Value;
                }
                catch (Exception)
                {
                    // Silent failure. Typically this means the dll hasn't been built with --enable-qsv
                    return false;
                }
            }
        }

        public static bool IsNVDecAvailable
        {
            get
            {
                try
                {
                    if (!IsNVEncH264Available)
                    {
                        return false;
                    }

                    if (isNVDecAvailable != null)
                    {
                        return isNVDecAvailable.Value;
                    }

                    isNVDecAvailable =  HBFunctions.hb_check_nvdec_available() > 0;

                    return isNVDecAvailable.Value;
                }
                catch (Exception)
                {
                    // Silent failure. Typically this means the dll hasn't been built with --enable-qsv
                    return false;
                }
            }
        }
    }
}
