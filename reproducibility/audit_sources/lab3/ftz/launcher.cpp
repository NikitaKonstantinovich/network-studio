// Audit-only diagnostic. Production sources and static library are unchanged.
#include <xmmintrin.h>
#define wmain original_network_studio_wmain
#define main original_network_studio_main
#include "../../../../cli.cpp"
#undef main
#undef wmain

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    const unsigned before = _mm_getcsr();
    _mm_setcsr(before | 0x8040u); // FTZ (bit 15), DAZ (bit 6).
    std::cerr << "AUDIT ONLY MXCSR before=" << before
              << " after=" << _mm_getcsr() << " FTZ+DAZ enabled\n";
    return original_network_studio_wmain(argc, argv);
}
#else
int main(int argc, char** argv) {
    const unsigned before = _mm_getcsr();
    _mm_setcsr(before | 0x8040u);
    std::cerr << "AUDIT ONLY MXCSR before=" << before
              << " after=" << _mm_getcsr() << " FTZ+DAZ enabled\n";
    return original_network_studio_main(argc, argv);
}
#endif
