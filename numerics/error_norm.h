#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#if defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace studio::numerical {
// The same coordinate-wise step-doubling norm, including both multiplications
// in its denominator. SSE2 processes two independent coordinates; it does not
// change precision, reassociate expressions, approximate division or omit work.
// Only nonnegative maxima are reduced, never floating-point sums.
inline double error_norm_range(const double* old,const double* next,const double* full,
                              size_t begin,size_t end) {
    double error=0;
#if defined(_M_X64) || defined(__SSE2__)
    const auto sign=_mm_set1_pd(-0.0),inf=_mm_set1_pd(std::numeric_limits<double>::infinity());
    auto maximum=_mm_setzero_pd();
    for(;end-begin>=2;begin+=2) {
        const auto a=_mm_loadu_pd(old+begin),b=_mm_loadu_pd(next+begin),c=_mm_loadu_pd(full+begin);
        const auto ab=_mm_andnot_pd(sign,b),ac=_mm_andnot_pd(sign,c);
        if(_mm_movemask_pd(_mm_and_pd(_mm_cmplt_pd(ab,inf),_mm_cmplt_pd(ac,inf)))!=3)
            return std::numeric_limits<double>::infinity();
        // MAXPD selects its second operand on ties/unordered input, matching
        // std::max(abs(old),abs(next)) with this operand order (even old=NaN).
        const auto scale=_mm_add_pd(_mm_set1_pd(1e-11),
            _mm_mul_pd(_mm_set1_pd(1e-8),_mm_max_pd(ab,_mm_andnot_pd(sign,a))));
        const auto ratio=_mm_div_pd(_mm_andnot_pd(sign,_mm_sub_pd(b,c)),_mm_mul_pd(_mm_set1_pd(15),scale));
        maximum=_mm_max_pd(ratio,maximum);
    }
    double lanes[2];_mm_storeu_pd(lanes,maximum);error=std::max(lanes[0],lanes[1]);
#endif
    for(;begin<end;++begin) {
        if(!std::isfinite(next[begin])||!std::isfinite(full[begin]))return std::numeric_limits<double>::infinity();
        const double scale=1e-11+1e-8*std::max(std::abs(old[begin]),std::abs(next[begin]));
        error=std::max(error,std::abs(next[begin]-full[begin])/(15*scale));
    }
    return error;
}
}
