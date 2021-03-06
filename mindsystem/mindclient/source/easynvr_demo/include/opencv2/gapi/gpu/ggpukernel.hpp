// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
//
// Copyright (C) 2018 Intel Corporation


#ifndef OPENCV_GAPI_GGPUKERNEL_HPP
#define OPENCV_GAPI_GGPUKERNEL_HPP

#include <vector>
#include <functional>
#include <map>
#include <unordered_map>

#include <opencv2/core/mat.hpp>
#include <opencv2/gapi/gcommon.hpp>
#include <opencv2/gapi/gkernel.hpp>
#include <opencv2/gapi/garg.hpp>

// FIXME: namespace scheme for backends?
namespace cv {

namespace gimpl
{
    // Forward-declare an internal class
    class GGPUExecutable;
} // namespace gimpl

namespace gapi
{
namespace gpu
{
    /**
     * \addtogroup gapi_std_backends G-API Standard backends
     * @{
     */
    /**
     * @brief Get a reference to GPU backend.
     *
     * At the moment, the GPU backend is built atop of OpenCV
     * "Transparent API" (T-API), see cv::UMat for details.
     *
     * @sa gapi_std_backends
     */
    GAPI_EXPORTS cv::gapi::GBackend backend();
    /** @} */
} // namespace gpu
} // namespace gapi


// Represents arguments which are passed to a wrapped GPU function
// FIXME: put into detail?
class GAPI_EXPORTS GGPUContext
{
public:
    // Generic accessor API
    template<typename T>
    const T& inArg(int input) { return m_args.at(input).get<T>(); }

    // Syntax sugar
    const cv::UMat&  inMat(int input);
    cv::UMat&  outMatR(int output); // FIXME: Avoid cv::Mat m = ctx.outMatR()

    const cv::gapi::own::Scalar& inVal(int input);
    cv::gapi::own::Scalar& outValR(int output); // FIXME: Avoid cv::gapi::own::Scalar s = ctx.outValR()
    template<typename T> std::vector<T>& outVecR(int output) // FIXME: the same issue
    {
        return outVecRef(output).wref<T>();
    }

protected:
    detail::VectorRef& outVecRef(int output);

    std::vector<GArg> m_args;
    std::unordered_map<std::size_t, GRunArgP> m_results;


    friend class gimpl::GGPUExecutable;
};

class GAPI_EXPORTS GGPUKernel
{
public:
    // This function is kernel's execution entry point (does the processing work)
    using F = std::function<void(GGPUContext &)>;

    GGPUKernel();
    explicit GGPUKernel(const F& f);

    void apply(GGPUContext &ctx);

protected:
    F m_f;
};

// FIXME: This is an ugly ad-hoc imlpementation. TODO: refactor

namespace detail
{
template<class T> struct gpu_get_in;
template<> struct gpu_get_in<cv::GMat>
{
    static cv::UMat    get(GGPUContext &ctx, int idx) { return ctx.inMat(idx); }
};
template<> struct gpu_get_in<cv::GScalar>
{
    static cv::Scalar get(GGPUContext &ctx, int idx) { return to_ocv(ctx.inVal(idx)); }
};
template<typename U> struct gpu_get_in<cv::GArray<U> >
{
    static const std::vector<U>& get(GGPUContext &ctx, int idx) { return ctx.inArg<VectorRef>(idx).rref<U>(); }
};
template<class T> struct gpu_get_in
{
    static T get(GGPUContext &ctx, int idx) { return ctx.inArg<T>(idx); }
};

struct tracked_cv_umat{
    //TODO Think if T - API could reallocate UMat to a proper size - how do we handle this ?
    //tracked_cv_umat(cv::UMat& m) : r{(m)}, original_data{m.getMat(ACCESS_RW).data} {}
    tracked_cv_umat(cv::UMat& m) : r{ (m) }, original_data{ nullptr } {}
    cv::UMat r;
    uchar* original_data;

    operator cv::UMat& (){ return r;}
    void validate() const{
        //if (r.getMat(ACCESS_RW).data != original_data)
        //{
        //    util::throw_error
        //        (std::logic_error
        //         ("OpenCV kernel output parameter was reallocated. \n"
        //          "Incorrect meta data was provided ?"));
        //}

    }
};

struct scalar_wrapper_gpu
{
    //FIXME reuse CPU (OpenCV) plugin code
    scalar_wrapper_gpu(cv::gapi::own::Scalar& s) : m_s{cv::gapi::own::to_ocv(s)}, m_org_s(s) {};
    operator cv::Scalar& () { return m_s; }
    void writeBack() const  { m_org_s = to_own(m_s); }

    cv::Scalar m_s;
    cv::gapi::own::Scalar& m_org_s;
};

template<typename... Outputs>
void postprocess_gpu(Outputs&... outs)
{
    struct
    {
        void operator()(tracked_cv_umat* bm) { bm->validate(); }
        void operator()(scalar_wrapper_gpu* sw) { sw->writeBack(); }
        void operator()(...) {                  }

    } validate;
    //dummy array to unfold parameter pack
    int dummy[] = { 0, (validate(&outs), 0)... };
    cv::util::suppress_unused_warning(dummy);
}

template<class T> struct gpu_get_out;
template<> struct gpu_get_out<cv::GMat>
{
    static tracked_cv_umat get(GGPUContext &ctx, int idx)
    {
        auto& r = ctx.outMatR(idx);
        return{ r };
    }
};
template<> struct gpu_get_out<cv::GScalar>
{
    static scalar_wrapper_gpu get(GGPUContext &ctx, int idx)
    {
        auto& s = ctx.outValR(idx);
        return{ s };
    }
};
template<typename U> struct gpu_get_out<cv::GArray<U> >
{
    static std::vector<U>& get(GGPUContext &ctx, int idx) { return ctx.outVecR<U>(idx);  }
};

template<typename, typename, typename>
struct GPUCallHelper;

// FIXME: probably can be simplified with std::apply or analogue.
template<typename Impl, typename... Ins, typename... Outs>
struct GPUCallHelper<Impl, std::tuple<Ins...>, std::tuple<Outs...> >
{
    template<typename... Inputs>
    struct call_and_postprocess
    {
        template<typename... Outputs>
        static void call(Inputs&&... ins, Outputs&&... outs)
        {
            //not using a std::forward on outs is deliberate in order to
            //cause compilation error, by tring to bind rvalue references to lvalue references
            Impl::run(std::forward<Inputs>(ins)..., outs...);

            postprocess_gpu(outs...);
        }
    };

    template<int... IIs, int... OIs>
    static void call_impl(GGPUContext &ctx, detail::Seq<IIs...>, detail::Seq<OIs...>)
    {
        //TODO: Make sure that OpenCV kernels do not reallocate memory for output parameters
        //by comparing it's state (data ptr) before and after the call.
        //Convert own::Scalar to cv::Scalar before call kernel and run kernel
        //convert cv::Scalar to own::Scalar after call kernel and write back results
        call_and_postprocess<decltype(gpu_get_in<Ins>::get(ctx, IIs))...>::call(gpu_get_in<Ins>::get(ctx, IIs)..., gpu_get_out<Outs>::get(ctx, OIs)...);
    }

    static void call(GGPUContext &ctx)
    {
        call_impl(ctx,
            typename detail::MkSeq<sizeof...(Ins)>::type(),
            typename detail::MkSeq<sizeof...(Outs)>::type());
    }
};

} // namespace detail

template<class Impl, class K>
class GGPUKernelImpl: public detail::GPUCallHelper<Impl, typename K::InArgs, typename K::OutArgs>
{
    using P = detail::GPUCallHelper<Impl, typename K::InArgs, typename K::OutArgs>;

public:
    using API = K;

    static cv::gapi::GBackend backend()  { return cv::gapi::gpu::backend(); }
    static cv::GGPUKernel     kernel()   { return GGPUKernel(&P::call);     }
};

#define GAPI_GPU_KERNEL(Name, API) struct Name: public cv::GGPUKernelImpl<Name, API>

} // namespace cv

#endif // OPENCV_GAPI_GGPUKERNEL_HPP
