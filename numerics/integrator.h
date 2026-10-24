#pragma once
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <vector>
#include "performance/profile.h"
#include "worker_pool.h"
#include "error_norm.h"
#include "integration_method.h"

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
// Selected nonlinear flow with exact independent linear coordinates at every
// stage. Only AdaptiveRK4 uses step doubling and accepts two half steps.
class Flow {
    Vector k1,k2,k3,k4,stage,full,scratch;
    struct Candidate { int index; double low,high; };
    std::vector<Candidate> candidates;
    void euler(const Vector& old,double h,Vector& out) {
        rhs(old,k1);out.resize(old.size());
        const size_t count=nonlinear_prefix?nonlinear_prefix:old.size();
        for(size_t i=0;i<count;++i)out[i]=old[i]+h*k1[i];
        exact(old,h,out);
    }
    void rk4(const Vector& old,double h,Vector& out) {
        if(nonlinear_prefix) {
            // exact() replaces every suffix coordinate independently of stage.
            // Keep identical nonlinear arithmetic; reuse the identical linear
            // values at stages 2/3 and at stage 4/the final RK combination.
            rhs(old,k1);
            for(size_t i=0;i<nonlinear_prefix;++i)stage[i]=old[i]+h*.5*k1[i];
            exact(old,h*.5,stage);rhs(stage,k2);
            for(size_t i=0;i<nonlinear_prefix;++i)stage[i]=old[i]+h*.5*k2[i];
            rhs(stage,k3);
            for(size_t i=0;i<nonlinear_prefix;++i)stage[i]=old[i]+h*k3[i];
            exact(old,h,stage);rhs(stage,k4);
            // stage is private scratch and distinct from old/out in every
            // rk4 caller. Its entire prefix and exact suffix are overwritten
            // before the next RHS; exchanging storage avoids a full-state copy.
            // Public sample() also accepts an initially empty external output.
            if(out.size()!=stage.size())out.resize(stage.size());
            out.swap(stage);
            for(size_t i=0;i<nonlinear_prefix;++i)out[i]=old[i]+h/6*(k1[i]+2*k2[i]+2*k3[i]+k4[i]);
            return;
        }
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
    IntegrationMethod method=IntegrationMethod::AdaptiveRK4;
    // Opt-in only: all remaining coordinates MUST have independent exact flow,
    // and exact() MUST leave this prefix unchanged. Zero keeps the general path.
    size_t nonlinear_prefix=0;
    std::function<void(const Vector&,Vector&)> rhs;
    std::function<void(const Vector&,double,Vector&)> exact;
    // Optional projection of sample() onto an independent threshold coordinate.
    std::function<double(const Vector&,double,int)> threshold_sample;
    // Independent projected roots only; worker callback MUST NOT dispatch into
    // this pool or use Flow's shared stage/next scratch. Null retains reference.
    WorkerPool* event_workers=nullptr;
    std::function<double(const Vector&,double,int,int)> worker_threshold_sample;
    // Optional disjoint-coordinate evaluation of the SAME step-doubling norm.
    std::function<double(const Vector&,const Vector&,const Vector&)> error_norm;
    Vector half,next;
    explicit Flow(size_t size=0):k1(size),k2(size),k3(size),k4(size),stage(size),full(size),scratch(size),half(size),next(size) {}
    // Reuse storage when projected root searches visit different indegrees.
    // No vector contents or callbacks are interpreted until sample()/trial().
    void resize(size_t size) {
        k1.resize(size);k2.resize(size);k3.resize(size);k4.resize(size);
        stage.resize(size);full.resize(size);scratch.resize(size);
        half.resize(size);next.resize(size);
    }
    void sample(const Vector& old,double h,Vector& out) {
        if(method==IntegrationMethod::Euler)euler(old,h,out);
        else if(method==IntegrationMethod::RK4){out.resize(old.size());rk4(old,h,out);}
        else {rk4(old,h*.5,scratch);rk4(scratch,h*.5,out);}
    }
    double trial(const Vector& old,double h) {
        STUDIO_SCOPE(trial);
        if(method!=IntegrationMethod::AdaptiveRK4) {
            sample(old,h,next);
            return std::numeric_limits<double>::quiet_NaN(); // no error estimate
        }
        rk4(old,h,full);rk4(old,h*.5,half);rk4(half,h*.5,next);
        if(error_norm)return error_norm(old,next,full);
        return error_norm_range(old.data(),next.data(),full.data(),0,old.size());
    }
    // Localize every candidate independently, then apply the earliest group.
    // This makes simultaneous-event membership independent of node iteration.
    double events(const Vector& old,double h,double clock,const std::vector<Threshold>& thresholds,
                  std::vector<int>& group) {
        STUDIO_SCOPE(thresholds);
        candidates.clear();
        for(int i=0;i<int(thresholds.size());++i) {
            const auto& e=thresholds[i];
            const double tol=threshold_tolerance(e.value);
            auto crossed=[&](double a,double b){return a<e.value && b>=e.value-tol && b>a;};
            if(method==IntegrationMethod::Euler) {
                if(e.armed&&crossed(old[e.coordinate],next[e.coordinate])) {
                    const double root=h*std::clamp((e.value-old[e.coordinate])/(next[e.coordinate]-old[e.coordinate]),0.,1.);
                    candidates.push_back({i,root,root});
                }
            } else if(method==IntegrationMethod::RK4) {
                if(e.armed&&crossed(old[e.coordinate],next[e.coordinate]))candidates.push_back({i,0,h});
                // RK stages are only hints for a possible interior excursion.
                // The negative bracket is resolved below by a selected-method
                // projection, never by accepting two sequential half steps.
                else if(e.armed&&old[e.coordinate]<e.value&&
                    std::max({old[e.coordinate]+h*.5*k1[e.coordinate],old[e.coordinate]+h*.5*k2[e.coordinate],
                              old[e.coordinate]+h*k3[e.coordinate]})>=e.value-tol)
                    candidates.push_back({i,-1,h});
            } else if(e.armed && crossed(old[e.coordinate],half[e.coordinate]))
                candidates.push_back({i,0,h*.5});
            else if((e.armed || half[e.coordinate]<e.value-tol) && crossed(half[e.coordinate],next[e.coordinate]))
                candidates.push_back({i,h*.5,h});
        }
        if(method==IntegrationMethod::RK4) {
            for(auto& c:candidates)if(c.low<0) {
                const auto& e=thresholds[c.index];double middle;
                if(threshold_sample)middle=threshold_sample(old,h*.5,e.coordinate);
                else {sample(old,h*.5,scratch);middle=scratch[e.coordinate];}
                c.low=0;
                if(middle>=e.value-threshold_tolerance(e.value))c.high=h*.5;
                else c.index=-1;
            }
            candidates.erase(std::remove_if(candidates.begin(),candidates.end(),[](const Candidate& c){return c.index<0;}),candidates.end());
        }
        profile::candidates(candidates.size());
        group.clear();if(candidates.empty())return h;
        const double tolerance=event_tolerance(clock);
        const bool parallel=event_workers&&worker_threshold_sample&&candidates.size()>=4;
        auto locate=[&](size_t begin,size_t end,int id) {
            for(size_t k=begin;k<end;++k) {
                auto& c=candidates[k];const auto& e=thresholds[c.index];
                for(int iter=0;iter<60 && c.high-c.low>tolerance;++iter) {
                    const double mid=c.low+(c.high-c.low)*.5;
                    double value;
                    if(parallel)value=worker_threshold_sample(old,mid,e.coordinate,id);
                    else if(threshold_sample)value=threshold_sample(old,mid,e.coordinate);
                    else {sample(old,mid,next);value=next[e.coordinate];}
                    if(value>=e.value-threshold_tolerance(e.value))c.high=mid;else c.low=mid;
                }
            }
        };
        if(parallel)event_workers->run(candidates.size(),locate);else locate(0,candidates.size(),0);
        double first=h;
        for(const auto& c:candidates)first=std::min(first,c.high);
        for(const auto& c:candidates)if(c.high<=first+2*tolerance)group.push_back(c.index);
        sample(old,first,next);
        return first;
    }
};
}
