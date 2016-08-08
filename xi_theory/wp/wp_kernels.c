/* File: wp_kernels.c */
/*
  This file is a part of the Corrfunc package
  Copyright (C) 2015-- Manodeep Sinha (manodeep@gmail.com)
  License: MIT LICENSE. See LICENSE file under the top-level
  directory at https://github.com/manodeep/Corrfunc/
*/


#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>

#include "function_precision.h"

#ifdef __AVX__
#include "avx_calls.h"

static inline int wp_avx_intrinsics(DOUBLE *x0, DOUBLE *y0, DOUBLE *z0, const int64_t N0,
                                    DOUBLE *x1, DOUBLE *y1, DOUBLE *z1, const int64_t N1, const int same_cell,
                                    const DOUBLE sqr_rpmax, const DOUBLE sqr_rpmin, const int nbin, const DOUBLE *rupp_sqr, const DOUBLE pimax,
                                    const DOUBLE off_xwrap, const DOUBLE off_ywrap, const DOUBLE off_zwrap
                                    ,DOUBLE *src_rpavg
                                    ,uint64_t *src_npairs)
{
    uint64_t *npairs = calloc(sizeof(*npairs), nbin);
    if(npairs == NULL) {
        perror(NULL);
        return EXIT_FAILURE;
    }
    AVX_FLOATS m_rupp_sqr[nbin];
    for(int i=0;i<nbin;i++) {
        m_rupp_sqr[i] = AVX_SET_FLOAT(rupp_sqr[i]);
    }
    const int32_t need_rpavg = src_rpavg != NULL;

    /* variables required for rpavg */
    union int8 {
        AVX_INTS m_ibin;
        int ibin[AVX_NVEC];
    };
    union float8{
        AVX_FLOATS m_Dperp;
        DOUBLE Dperp[AVX_NVEC];
    };
    AVX_FLOATS m_kbin[nbin];
    DOUBLE rpavg[nbin];
    if(need_rpavg) {
        for(int i=0;i<nbin;i++) {
            m_kbin[i] = AVX_SET_FLOAT((DOUBLE) i);
            rpavg[i] = ZERO;
        }
    }

    
    for(int64_t i=0;i<N0;i++) {
        const DOUBLE xpos = *x0++ + off_xwrap;
        const DOUBLE ypos = *y0++ + off_ywrap;
        const DOUBLE zpos = *z0++ + off_zwrap;

        DOUBLE *localz1 = (DOUBLE *) z1;

        int64_t j=0;
        if(same_cell == 1) {
            j = i+1;
            localz1 += j;
        } else {
            while(j < N1){
                const DOUBLE dz = *localz1++ - zpos;
                if(dz > -pimax) break;
                j++;
            }
            localz1--;
        }
        DOUBLE *localx1 = x1 + j;
        DOUBLE *localy1 = y1 + j;

        for(;j<=(N1 - AVX_NVEC);j+=AVX_NVEC) {
            const AVX_FLOATS m_xpos    = AVX_SET_FLOAT(xpos);
            const AVX_FLOATS m_ypos    = AVX_SET_FLOAT(ypos);
            const AVX_FLOATS m_zpos    = AVX_SET_FLOAT(zpos);
            
            union int8 union_rpbin;
            union float8 union_mDperp;

            const AVX_FLOATS m_x1 = AVX_LOAD_FLOATS_UNALIGNED(localx1);
            const AVX_FLOATS m_y1 = AVX_LOAD_FLOATS_UNALIGNED(localy1);
            const AVX_FLOATS m_z1 = AVX_LOAD_FLOATS_UNALIGNED(localz1);
            
            localx1 += AVX_NVEC;//this might actually exceed the allocated range but we will never dereference that
            localy1 += AVX_NVEC;
            localz1 += AVX_NVEC;

            const AVX_FLOATS m_pimax = AVX_SET_FLOAT(pimax);
            const AVX_FLOATS m_sqr_rpmax = m_rupp_sqr[nbin-1];
            const AVX_FLOATS m_sqr_rpmin = m_rupp_sqr[0];
            
            const AVX_FLOATS m_zdiff = AVX_SUBTRACT_FLOATS(m_z1, m_zpos);//z2[j:j+NVEC-1] - z1
            const AVX_FLOATS m_sqr_xdiff = AVX_SQUARE_FLOAT(AVX_SUBTRACT_FLOATS(m_xpos,m_x1));//(x0 - x[j])^2
            const AVX_FLOATS m_sqr_ydiff = AVX_SQUARE_FLOAT(AVX_SUBTRACT_FLOATS(m_ypos,m_y1));//(y0 - y[j])^2
            AVX_FLOATS r2  = AVX_ADD_FLOATS(m_sqr_xdiff,m_sqr_ydiff);
            
            AVX_FLOATS m_mask_left;
            
            //Do all the distance cuts using masks here in new scope
            {
                //the z2 arrays are sorted in increasing order. which means
                //the z2 value will increase in any future iteration of j.
                //that implies the zdiff values are also monotonically increasing
                //Therefore, if none of the zdiff values are less than pimax, then
                //no future iteration in j can produce a zdiff value less than pimax.
                AVX_FLOATS m_mask_pimax = AVX_COMPARE_FLOATS(m_zdiff,m_pimax,_CMP_LT_OS);
                if(AVX_TEST_COMPARISON(m_mask_pimax) == 0) {
                    //None of the dz^2 values satisfies dz^2 < pimax^2
                    // => no pairs can be added -> continue and process the next NVEC
                    j=N1;
                    break;
                }
                
                const AVX_FLOATS m_rpmax_mask = AVX_COMPARE_FLOATS(r2, m_sqr_rpmax, _CMP_LT_OS);
                const AVX_FLOATS m_rpmin_mask = AVX_COMPARE_FLOATS(r2, m_sqr_rpmin, _CMP_GE_OS);
                const AVX_FLOATS m_rp_mask = AVX_BITWISE_AND(m_rpmax_mask,m_rpmin_mask);
                
                //Create a combined mask by bitwise and of m1 and m_mask_left.
                //This gives us the mask for all sqr_rpmin <= r2 < sqr_rpmax
                m_mask_left = AVX_BITWISE_AND(m_mask_pimax,m_rp_mask);
                
                //If not, continue with the next iteration of j-loop
                if(AVX_TEST_COMPARISON(m_mask_left) == 0) {
                    continue;
                }
                
                //There is some r2 that satisfies sqr_rpmin <= r2 < sqr_rpmax && 0.0 <= dz^2 < pimax^2.
                r2 = AVX_BLEND_FLOATS_WITH_MASK(m_sqr_rpmax, r2, m_mask_left);
            }

            AVX_FLOATS m_rpbin = AVX_SET_FLOAT(ZERO);
            if(need_rpavg) {
                union_mDperp.m_Dperp = AVX_SQRT_FLOAT(r2);
            }
            
            //Loop backwards through nbins. m_mask_left contains all the points that are less than rpmax
            for(int kbin=nbin-1;kbin>=1;kbin--) {
                const AVX_FLOATS m1 = AVX_COMPARE_FLOATS(r2,m_rupp_sqr[kbin-1],_CMP_GE_OS);
                const AVX_FLOATS m_bin_mask = AVX_BITWISE_AND(m1,m_mask_left);
                const int test2  = AVX_TEST_COMPARISON(m_bin_mask);
                npairs[kbin] += AVX_BIT_COUNT_INT(test2);
                if(need_rpavg) {
                    m_rpbin = AVX_BLEND_FLOATS_WITH_MASK(m_rpbin,m_kbin[kbin], m_bin_mask);
                }

                m_mask_left = AVX_COMPARE_FLOATS(r2,m_rupp_sqr[kbin-1],_CMP_LT_OS);
                const int test3 = AVX_TEST_COMPARISON(m_mask_left);
                if(test3 == 0) {
                    break;
                }
            }
            
            if(need_rpavg) {
                union_rpbin.m_ibin = AVX_TRUNCATE_FLOAT_TO_INT(m_rpbin);
                //protect the unroll pragma in case compiler is not icc.
#if  __INTEL_COMPILER
#pragma unroll(AVX_NVEC)
#endif
                for(int jj=0;jj<AVX_NVEC;jj++) {
                    const int kbin = union_rpbin.ibin[jj];
                    const DOUBLE r = union_mDperp.Dperp[jj];
                    rpavg[kbin] += r;
                }
            } //OUTPUT_RPAVG

        }//end of j-loop
        
        //remainder loop 
        for(;j<N1;j++){
            const DOUBLE dz = *localz1++ - zpos;
            const DOUBLE dx = *localx1++ - xpos;
            const DOUBLE dy = *localy1++ - ypos;

            if(dz >= pimax) {
                break;
            } 
            
            const DOUBLE r2 = dx*dx + dy*dy;
            if(r2 >= sqr_rpmax || r2 < sqr_rpmin) {
                continue;
            }
            
            const DOUBLE r = need_rpavg ? SQRT(r2):ZERO;
            for(int kbin=nbin-1;kbin>=1;kbin--) {
                if(r2 >= rupp_sqr[kbin-1]) {
                    npairs[kbin]++;
                    if(need_rpavg) {
                        rpavg[kbin] += r;
                    }
                    break;
                }
            }
        }//remainder loop over second set of particles
    }//loop over first set of particles

    for(int i=0;i<nbin;i++) {
        src_npairs[i] += npairs[i];
        if(need_rpavg) {
            src_rpavg[i]  += rpavg[i];
        }
    }
    free(npairs);

    return EXIT_SUCCESS;
}
#endif //__AVX__



#ifdef __SSE4_2__
#include "sse_calls.h"

static inline int wp_sse_intrinsics(DOUBLE *x0, DOUBLE *y0, DOUBLE *z0, const int64_t N0,
                                    DOUBLE *x1, DOUBLE *y1, DOUBLE *z1, const int64_t N1, const int same_cell, 
                                    const DOUBLE sqr_rpmax, const DOUBLE sqr_rpmin, const int nbin, const DOUBLE rupp_sqr[] , const DOUBLE pimax,
                                    const DOUBLE off_xwrap, const DOUBLE off_ywrap, const DOUBLE off_zwrap
                                    ,DOUBLE *src_rpavg
                                    ,uint64_t *src_npairs)
{
    SSE_FLOATS m_rupp_sqr[nbin];
    for(int i=0;i<nbin;i++) {
        m_rupp_sqr[i] = SSE_SET_FLOAT(rupp_sqr[i]);
    }
    const int32_t need_rpavg = src_rpavg != NULL;
    union int4{
        SSE_INTS m_ibin;
        int ibin[SSE_NVEC];
    };
    union float4{
        SSE_FLOATS m_Dperp;
        DOUBLE Dperp[SSE_NVEC];
    };
    
    SSE_FLOATS m_kbin[nbin];
    DOUBLE rpavg[nbin];
    if(need_rpavg) {
        for(int i=0;i<nbin;i++) {
            m_kbin[i] = SSE_SET_FLOAT((DOUBLE) i);
            rpavg[i] = ZERO;
        }
    }
    
    uint64_t *npairs = calloc(sizeof(*npairs), nbin);
    if(npairs == NULL) {
        perror(NULL);
        return EXIT_FAILURE;
    }
    for(int64_t i=0;i<N0;i++) {
        const DOUBLE xpos = *x0++ + off_xwrap;
        const DOUBLE ypos = *y0++ + off_ywrap;
        const DOUBLE zpos = *z0++ + off_zwrap;


        DOUBLE *localz1 = z1;
        int64_t j=0;
        if(same_cell == 1) {
            j = i+1;
            localz1 += j;
        } else {
            while(j < N1){
                const DOUBLE dz = *localz1++ - zpos;
                if(dz > -pimax) break;
                j++;
            }
            localz1--;
        }
        DOUBLE *localx1 = x1 + j;
        DOUBLE *localy1 = y1 + j;

		for(;j<=(N1 - SSE_NVEC);j+=SSE_NVEC){
            union int4 union_rpbin;
            union float4 union_mDperp;

            const SSE_FLOATS m_xpos = SSE_SET_FLOAT(xpos);
            const SSE_FLOATS m_ypos = SSE_SET_FLOAT(ypos);
            const SSE_FLOATS m_zpos = SSE_SET_FLOAT(zpos);

            const SSE_FLOATS m_x1 = SSE_LOAD_FLOATS_UNALIGNED(localx1);
			const SSE_FLOATS m_y1 = SSE_LOAD_FLOATS_UNALIGNED(localy1);
			const SSE_FLOATS m_z1 = SSE_LOAD_FLOATS_UNALIGNED(localz1);

			localx1 += SSE_NVEC;
			localy1 += SSE_NVEC;
			localz1 += SSE_NVEC;

            const SSE_FLOATS m_pimax = SSE_SET_FLOAT(pimax);
			const SSE_FLOATS m_sqr_rpmax = SSE_SET_FLOAT(sqr_rpmax);
			const SSE_FLOATS m_sqr_rpmin = SSE_SET_FLOAT(sqr_rpmin);
			

			const SSE_FLOATS m_sqr_xdiff = SSE_SQUARE_FLOAT(SSE_SUBTRACT_FLOATS(m_x1, m_xpos));
            const SSE_FLOATS m_sqr_ydiff = SSE_SQUARE_FLOAT(SSE_SUBTRACT_FLOATS(m_y1, m_ypos));
            const SSE_FLOATS m_zdiff = SSE_SUBTRACT_FLOATS(m_z1, m_zpos);
            
            SSE_FLOATS r2  = SSE_ADD_FLOATS(m_sqr_xdiff,m_sqr_ydiff);
            SSE_FLOATS m_mask_left;
			{
                const SSE_FLOATS m_pimax_mask = SSE_COMPARE_FLOATS_LT(m_zdiff,m_pimax);
                if(SSE_TEST_COMPARISON(m_pimax_mask) == 0) {
                    j = N1;
                    break;
                }
                
                const SSE_FLOATS m_rpmin_mask = SSE_COMPARE_FLOATS_GE(r2, m_sqr_rpmin);
                const SSE_FLOATS m_rpmax_mask = SSE_COMPARE_FLOATS_LT(r2,m_sqr_rpmax);
                const SSE_FLOATS m_rp_mask = SSE_BITWISE_AND(m_rpmin_mask, m_rpmax_mask);
                m_mask_left = SSE_BITWISE_AND(m_pimax_mask, m_rp_mask);
				if(SSE_TEST_COMPARISON(m_mask_left) == 0) {
					continue;
				}
				r2 = SSE_BLEND_FLOATS_WITH_MASK(m_sqr_rpmax, r2, m_mask_left);
            }
                
            SSE_FLOATS m_rpbin = SSE_SET_FLOAT(ZERO);
            if(need_rpavg) {
                union_mDperp.m_Dperp = SSE_SQRT_FLOAT(r2);
            }
            
			for(int kbin=nbin-1;kbin>=1;kbin--) {
				SSE_FLOATS m1 = SSE_COMPARE_FLOATS_GE(r2,m_rupp_sqr[kbin-1]);
				SSE_FLOATS m_bin_mask = SSE_BITWISE_AND(m1,m_mask_left);
				m_mask_left = SSE_COMPARE_FLOATS_LT(r2,m_rupp_sqr[kbin-1]);
				int test2  = SSE_TEST_COMPARISON(m_bin_mask);
				npairs[kbin] += SSE_BIT_COUNT_INT(test2);
                if(need_rpavg) {
                    m_rpbin = SSE_BLEND_FLOATS_WITH_MASK(m_rpbin,m_kbin[kbin], m_bin_mask);
                }
				int test3 = SSE_TEST_COMPARISON(m_mask_left);
				if(test3 == 0) {
					break;
				}
			}

            if(need_rpavg) {
                union_rpbin.m_ibin = SSE_TRUNCATE_FLOAT_TO_INT(m_rpbin);
                //protect the unroll pragma in case compiler is not icc.
#if  __INTEL_COMPILER
#pragma unroll(SSE_NVEC)
#endif
                for(int jj=0;jj<SSE_NVEC;jj++) {
                    const int kbin = union_rpbin.ibin[jj];
                    const DOUBLE r = union_mDperp.Dperp[jj];
                    rpavg[kbin] += r;
                }
            } //rpavg
		}//j loop over N1, increments of SSE_NVEC			

		for(;j<N1;j++) {
			const DOUBLE dx = *localx1++ - xpos;
			const DOUBLE dy = *localy1++ - ypos;
			const DOUBLE dz = *localz1++ - zpos;
            if(dz >= pimax) break;
            
			const DOUBLE r2 = dx*dx + dy*dy;
			if(r2 >= sqr_rpmax || r2 < sqr_rpmin) continue;
            const DOUBLE r = need_rpavg ? SQRT(r2):ZERO;
			for(int kbin=nbin-1;kbin>=1;kbin--){
				if(r2 >= rupp_sqr[kbin-1]) {
					npairs[kbin]++;
                    if(need_rpavg) {
                        rpavg[kbin] += r;
                    }
					break;
				}
			}//searching for kbin loop
		}
    }

	for(int i=0;i<nbin;i++) {
		src_npairs[i] += npairs[i];
        if(need_rpavg) {
            src_rpavg[i] += rpavg[i];
        }
	}
	free(npairs);

    return EXIT_SUCCESS;
}

#endif //__SSE4_2__



//Fallback code that should always compile
static inline int wp_fallback(DOUBLE *x0, DOUBLE *y0, DOUBLE *z0, const int64_t N0,
                               DOUBLE *x1, DOUBLE *y1, DOUBLE *z1, const int64_t N1, const int same_cell, 
                               const DOUBLE sqr_rpmax, const DOUBLE sqr_rpmin, const int nbin, const DOUBLE rupp_sqr[] , const DOUBLE pimax,
                               const DOUBLE off_xwrap, const DOUBLE off_ywrap, const DOUBLE off_zwrap
                               ,DOUBLE *src_rpavg
                               ,uint64_t *src_npairs)
{

    /*----------------- FALLBACK CODE --------------------*/
    uint64_t npairs[nbin];
    for(int i=0;i<nbin;i++) {
        npairs[i]=0;
    }
    const int32_t need_rpavg = src_rpavg != NULL;
    DOUBLE rpavg[nbin];
    if(need_rpavg) {
        for(int i=0;i<nbin;i++) {
            rpavg[i]=0;
        }
    }

    /* naive implementation that is guaranteed to compile */
    for(int64_t i=0;i<N0;i++) {
        const DOUBLE xpos = *x0++ + off_xwrap;
        const DOUBLE ypos = *y0++ + off_ywrap;
        const DOUBLE zpos = *z0++ + off_zwrap;

        DOUBLE *localz1 = (DOUBLE *) z1; 
        int64_t j=0;
        if(same_cell == 1) {
            j = i+1;
            localz1 += j;
        } else {
            while(j < N1){
                const DOUBLE dz = *localz1++ - zpos;
                if(dz > -pimax) break;
                j++;
            }
            localz1--;
        }
        DOUBLE *localx1 = x1 + j; 
        DOUBLE *localy1 = y1 + j;

        
        for(;j<N1;j++) {
            const DOUBLE dx = *localx1++ - xpos;
            const DOUBLE dy = *localy1++ - ypos;
            const DOUBLE dz = *localz1++ - zpos;
            if(dz >=pimax) break;

            const DOUBLE r2 = dx*dx + dy*dy;
            if(r2 >= sqr_rpmax || r2 < sqr_rpmin) continue;
            const DOUBLE r = need_rpavg ? SQRT(r2):ZERO;

            for(int kbin=nbin-1;kbin>=1;kbin--){
                if(r2 >= rupp_sqr[kbin-1]) {
                    npairs[kbin]++;
                    if(need_rpavg) {
                        rpavg[kbin] += r;
                    }
                    break;
                }
            }//searching for kbin loop                                                               
        }
    }

    for(int i=0;i<nbin;i++) {
        src_npairs[i] += npairs[i];
        if(need_rpavg) {
            src_rpavg[i] += rpavg[i];
        }
    }

    return EXIT_SUCCESS;
    /*----------------- FALLBACK CODE --------------------*/
}
