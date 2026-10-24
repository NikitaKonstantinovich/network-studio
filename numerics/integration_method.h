#pragma once
#include <array>
#include <stdexcept>
#include <string>
#include <string_view>

namespace studio {
enum class IntegrationMethod { AdaptiveRK4, RK4, Euler };
struct IntegrationMethodInfo { IntegrationMethod method; const char* argument; const char* label; };
inline constexpr std::array<IntegrationMethodInfo,3> IntegrationMethods{{
    {IntegrationMethod::AdaptiveRK4,"adaptive-rk4","Адаптивный RK4"},
    {IntegrationMethod::RK4,"rk4","RK4 без оценки ошибки"},
    {IntegrationMethod::Euler,"euler","Явный Эйлер"}
}};
inline const IntegrationMethodInfo& integration_method_info(IntegrationMethod method) {
    for(const auto& info:IntegrationMethods)if(info.method==method)return info;
    throw std::invalid_argument("Unknown integration method; expected adaptive-rk4, rk4 or euler.");
}
inline IntegrationMethod parse_integration_method(std::string_view value) {
    for(const auto& info:IntegrationMethods)if(value==info.argument)return info.method;
    throw std::invalid_argument("Unknown integration method '"+std::string(value)+"'; expected adaptive-rk4, rk4 or euler.");
}
}
