#pragma once
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <vector>

namespace studio::numerical {
using Vector = std::vector<double>;
struct Threshold { int coordinate; double value; bool armed = true; };
inline double threshold_tolerance(double value) {
    return 16*std::numeric_limits<double>::epsilon()*std::max(1.,std::abs(value));
}
// Absolute event tolerance in seconds, also bounded by clock resolution.
inline double event_tolerance(double time) {
    return std::max(1e-13, 8*std::numeric_limits<double>::epsilon()*std::max(1.,std::abs(time)));
}
// Integral exp(-a*(h-s))*exp(-b*s) ds, including coincident rates.
inline double convolution(double a,double b,double h) {
    const double delta=std::abs(a-b);
    return std::exp(-std::min(a,b)*h)*(delta==0 ? h : -std::expm1(-delta*h)/delta);
}
// Exact y -> z -> x resource transfer, without subtracting two nearly equal
// pools when h is small. The Taylor series is the integral of two exponentials.
inline double recovered(double a,double b,double h) {
    const double A=a*h,B=b*h;
    if(std::max(A,B)<.1) {
        long double sum=0,poly=1,factor=static_cast<long double>(A)*B/2;
        long double power_b=1;
        for(int k=0;k<18;++k) {
            if(k) { power_b*=B;poly=A*poly+power_b;factor/=(k+2); }
            sum+=(k%2 ? -1 : 1)*factor*poly;
        }
        return double(sum);
    }
    return -std::expm1(-b*h)-b*convolution(a,b,h);
}
// Fourth-order nonlinear flow with exact independent linear coordinates at
// EVERY stage. Two half steps provide the accepted state; step doubling checks
// the coupled quadrature as well as the nonlinear equations (no extrapolation).
class Flow {
    Vector k1,k2,k3,k4,stage,full,scratch;
    void rk4(const Vector& old,double h,Vector& out) {
        rhs(old,k1);
        for(size_t i=0;i<old.size();++i)stage[i]=old[i]+h*.5*k1[i];
        exact(old,h*.5,stage);rhs(stage,k2);
        for(size_t i=0;i<old.size();++i)stage[i]=old[i]+h*.5*k2[i];
        exact(old,h*.5,stage);rhs(stage,k3);
        for(size_t i=0;i<old.size();++i)stage[i]=old[i]+h*k3[i];
        exact(old,h,stage);rhs(stage,k4);
        for(size_t i=0;i<old.size();++i)out[i]=old[i]+h/6*(k1[i]+2*k2[i]+2*k3[i]+k4[i]);
        exact(old,h,out);
    }
public:
    std::function<void(const Vector&,Vector&)> rhs;
    std::function<void(const Vector&,double,Vector&)> exact;
    Vector half,next;
    explicit Flow(size_t size=0):k1(size),k2(size),k3(size),k4(size),stage(size),full(size),scratch(size),half(size),next(size) {}
    void sample(const Vector& old,double h,Vector& out) {
        rk4(old,h*.5,scratch);rk4(scratch,h*.5,out);
    }
    double trial(const Vector& old,double h) {
        rk4(old,h,full);rk4(old,h*.5,half);rk4(half,h*.5,next);
        double error=0;
        for(size_t i=0;i<old.size();++i) {
            if(!std::isfinite(next[i]) || !std::isfinite(full[i]))return std::numeric_limits<double>::infinity();
            const double scale=1e-11+1e-8*std::max(std::abs(old[i]),std::abs(next[i]));
            error=std::max(error,std::abs(next[i]-full[i])/(15*scale));
        }
        return error;
    }
    // Localize every candidate independently, then apply the earliest group.
    // This makes simultaneous-event membership independent of node iteration.
    double events(const Vector& old,double h,double clock,const std::vector<Threshold>& thresholds,
                  std::vector<int>& group) {
        struct Candidate { int index; double low,high; };
        std::vector<Candidate> candidates;
        for(int i=0;i<int(thresholds.size());++i) {
            const auto& e=thresholds[i];
            const double tol=threshold_tolerance(e.value);
            auto crossed=[&](double a,double b){return a<e.value && b>=e.value-tol && b>a;};
            if(e.armed && crossed(old[e.coordinate],half[e.coordinate]))
                candidates.push_back({i,0,h*.5});
            else if((e.armed || half[e.coordinate]<e.value-tol) && crossed(half[e.coordinate],next[e.coordinate]))
                candidates.push_back({i,h*.5,h});
        }
        group.clear();if(candidates.empty())return h;
        const double tolerance=event_tolerance(clock);
        double first=h;
        for(auto& c:candidates) {
            const auto& e=thresholds[c.index];
            for(int iter=0;iter<60 && c.high-c.low>tolerance;++iter) {
                const double mid=c.low+(c.high-c.low)*.5;
                sample(old,mid,next);
                if(next[e.coordinate]>=e.value-threshold_tolerance(e.value))c.high=mid;else c.low=mid;
            }
            first=std::min(first,c.high);
        }
        for(const auto& c:candidates)if(c.high<=first+2*tolerance)group.push_back(c.index);
        sample(old,first,next);
        return first;
    }
};
}
