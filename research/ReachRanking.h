#pragma once
#include <cmath>
#include <stdexcept>
namespace reach {
// Shared by the live scheduler and a explicitly restricted finite-model adapter.
// Probability concerns one short-horizon image use, NOT terminal task success.
inline double ReachShortScore(double probability,double ip,double lambda,double scale=65536.,double value=1.){
    if(!std::isfinite(probability)||probability<0||probability>1+1e-12||!std::isfinite(ip)||ip<0||!std::isfinite(lambda)||lambda<0||lambda>3||!std::isfinite(scale)||scale<=0||!std::isfinite(value)||value<=0||value>4)
        throw std::runtime_error("invalid ranking input");
    return value*probability-lambda*ip/scale;
}
}
