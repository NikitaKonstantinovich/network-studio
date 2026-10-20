#include "numerics/error_norm.h"
#include <array>
#include <cstring>
#include <iostream>
#include <random>
#include <vector>
using namespace studio::numerical;
double reference(const double* a,const double* b,const double* c,size_t begin,size_t end) {
    double error=0;
    for(size_t i=begin;i<end;++i) {
        if(!std::isfinite(b[i])||!std::isfinite(c[i]))return std::numeric_limits<double>::infinity();
        const double scale=1e-11+1e-8*std::max(std::abs(a[i]),std::abs(b[i]));
        error=std::max(error,std::abs(b[i]-c[i])/(15*scale));
    }
    return error;
}
int main() {try {
    std::mt19937_64 random(20261020);size_t checks=0;
    auto check=[&](const std::vector<double>& a,const std::vector<double>& b,const std::vector<double>& c,size_t begin,size_t end) {
        const auto expected=reference(a.data(),b.data(),c.data(),begin,end);
        const auto actual=error_norm_range(a.data(),b.data(),c.data(),begin,end);
        ++checks;if(std::memcmp(&expected,&actual,sizeof(double)))throw std::runtime_error("Norm changed bits");
    };
    for(size_t size:{0,1,2,3,7,16,17,101,62164}) {
        std::vector<double>a(size),b(size),c(size);
        for(int repeat=0;repeat<40;++repeat) {
            for(size_t i=0;i<size;++i) {
                uint64_t x=random(),y=random(),z=random();
                std::memcpy(&a[i],&x,8);std::memcpy(&b[i],&y,8);std::memcpy(&c[i],&z,8);
                // Also exercise finite, near-equal values instead of only
                // overwhelming infinity norms from arbitrary binary64 bits.
                if(repeat%2) {a[i]=(int64_t(x)%1000000)*.0001;b[i]=a[i]+(int64_t(y)%100)*1e-12;c[i]=a[i]+(int64_t(z)%100)*1e-12;}
            }
            check(a,b,c,0,size);
            for(int count:{1,2,4,6,8,12,16})for(int k=0;k<count;++k)check(a,b,c,size*k/count,size*(k+1)/count);
        }
    }
    const double inf=std::numeric_limits<double>::infinity(),nan=std::numeric_limits<double>::quiet_NaN();
    const std::array<double,12> special={0.,-0.,1.,-1.,1e-300,-1e-300,1e300,-1e300,inf,-inf,nan,std::numeric_limits<double>::denorm_min()};
    for(double a:special)for(double b:special)for(double c:special) {
        check({a,1,2},{b,1,2},{c,1,2},0,3);check({1,a},{1,b},{1,c},0,2);
    }
    std::cout<<checks<<" bit-exact norm checks passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
