// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include "core/providers/cuda/cuda_common.h"
#include "core/framework/stream_handles.h"
#include "core/providers/cuda/cuda_execution_provider.h"
#include "core/providers/cuda/cuda_fwd.h"
#include "core/platform/ort_mutex.h"
#include "core/providers/cuda/cuda_stream_handle.h"

namespace onnxruntime {
namespace cuda {

// -----------------------------------------------------------------------
// Base class for CUDA kernels
// -----------------------------------------------------------------------
class CudaKernel : public OpKernel {
 public:
  explicit CudaKernel(const OpKernelInfo& info)
      : OpKernel(info),
        // Is this OK to have a non-const execution provider?
        provider_(const_cast<CUDAExecutionProvider*>(static_cast<const CUDAExecutionProvider*>(info.GetExecutionProvider()))) {
  }

  Status Compute(OpKernelContext* p_op_kernel_context) const override {
    auto s = ComputeInternal(p_op_kernel_context);
    // use this to precisely locate the node where CUDA failure comes from
    if (cudaSuccess != cudaDeviceSynchronize())
      ORT_THROW("Error when sync cuda device!");
    if (s.IsOK()) {
      auto err = cudaGetLastError();
      if (err != cudaSuccess) {
        return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "CUDA error ", cudaGetErrorName(err), ":", cudaGetErrorString(err));
      }
    }
    return s;
  }

  virtual Status ComputeInternal(OpKernelContext* p_op_kernel_context) const = 0;

  template <typename T>
  inline IAllocatorUniquePtr<T> AllocateBufferOnCPUPinned(size_t count_or_bytes) const {
    AllocatorPtr allocator = provider_->GetAllocator(DEFAULT_CPU_ALLOCATOR_DEVICE_ID, OrtMemTypeCPU);
    if (!allocator)
      return nullptr;
    return IAllocator::MakeUniquePtr<T>(allocator, count_or_bytes);
  }

  template <typename T>
  inline IAllocatorUniquePtr<T> GetScratchBuffer(size_t count_or_bytes, onnxruntime::Stream* stream) const {
    return provider_->GetScratchBuffer<T>(count_or_bytes, stream, WaitCudaNotificationOnDevice);
  }

  // Different from GetScratchBuffer which use IAllocator::Alloc() to allocate memory,
  // this GetTransientScratchBuffer will call IAllocator::Reserve() to allocate memory.
  // IAllocator::Reserve() optionally implement some allocation logic that by-passes any arena-based
  // logic (or similar for different allocator) that may be housed in the Alloc() implementation.
  template <typename T>
  inline IAllocatorUniquePtr<T> GetTransientScratchBuffer(size_t count_or_bytes) const {
    return provider_->GetTransientScratchBuffer<T>(count_or_bytes);
  }

  inline void AddDeferredReleaseCPUPtr(void* p, onnxruntime::Stream* ort_stream) const {
    ORT_ENFORCE(ort_stream->device.Type() == OrtDevice::GPU);
    auto* cuda_ep_stream = static_cast<CudaStream*>(ort_stream);
    cuda_ep_stream->EnqueDeferredCPUBuffer(p);
  }

  const cudaDeviceProp& GetDeviceProp() const { return provider_->GetDeviceProp(); }

  inline cudaStream_t Stream(OpKernelContext* ctx) const {
    auto* stream = ctx->GetComputeStream();
    return stream ? static_cast<cudaStream_t>(stream->handle) : nullptr;
  }

  inline cudnnHandle_t GetCudnnHandle(OpKernelContext* ctx) const {
    return GetCudnnHandle(ctx->GetComputeStream());
  }

  static inline cudnnHandle_t GetCudnnHandle(onnxruntime::Stream* stream) {
    ORT_ENFORCE(stream->device.Type() == OrtDevice::GPU);
    return static_cast<CudaStream*>(stream)->cudnn_handle_;
  }

  inline cublasHandle_t GetCublasHandle(OpKernelContext* ctx) const {
    return GetCublasHandle(ctx->GetComputeStream());
  }

  static inline cublasHandle_t GetCublasHandle(onnxruntime::Stream* stream) {
    ORT_ENFORCE(stream->device.Type() == OrtDevice::GPU);
    return static_cast<CudaStream*>(stream)->cublas_handle_;
  }

  inline onnxruntime::Stream* OrtStream(OpKernelContext* ctx) const {
    return ctx->GetComputeStream();
  }

  // To support cudaMemcpyAsync, the cpu memory should be allocated in pinned memory
  // and it can only be released after the copy has finished
  template <typename T>
  class CudaAsyncBuffer {
   public:
    CudaAsyncBuffer(const CudaKernel* op_kernel) : gpu_copy_(nullptr), count_(0), op_kernel_(op_kernel) {}

    CudaAsyncBuffer(const CudaKernel* op_kernel, size_t count) : CudaAsyncBuffer(op_kernel) {
      AllocCpuPtr(count);
    }

    CudaAsyncBuffer(const CudaKernel* op_kernel, const T& value, size_t count)
        : CudaAsyncBuffer(op_kernel, count) {
      T* p = CpuPtr();
      for (size_t i = 0; i != count; ++i) {
        *p++ = value;
      }
    }

    CudaAsyncBuffer(const CudaKernel* op_kernel, gsl::span<T const> vec) : CudaAsyncBuffer(op_kernel, vec.size()) {
      memcpy(CpuPtr(), vec.data(), vec.size() * sizeof(T));
    }

    void AllocCpuPtr(size_t count) {
      cpu_pinned_copy_ = op_kernel_->AllocateBufferOnCPUPinned<T>(count);
      if (cpu_pinned_copy_ == nullptr)
        throw std::runtime_error("alloc failed");
      count_ = count;
    }

    Status CopyToGpu(onnxruntime::Stream* stream) {
      if (cpu_pinned_copy_) {
        gpu_copy_ = op_kernel_->GetScratchBuffer<T>(count_, stream);
        cudaStream_t cuda_stream = stream ? static_cast<cudaStream_t>(stream->handle) : nullptr;
        CUDA_RETURN_IF_ERROR(cudaMemcpyAsync(gpu_copy_.get(), cpu_pinned_copy_.get(), count_ * sizeof(T), cudaMemcpyHostToDevice,
                                             cuda_stream));
        op_kernel_->AddDeferredReleaseCPUPtr(cpu_pinned_copy_.release(), stream);
      }
      return Status::OK();
    }

    T* CpuPtr() const {
      return cpu_pinned_copy_.get();
    }

    gsl::span<T> CpuSpan() const {
      return gsl::span<T>(CpuPtr(), count_);
    }

    T* GpuPtr() const {
      return gpu_copy_.get();
    }

    size_t count() const {
      return count_;
    }

   protected:
    IAllocatorUniquePtr<T> gpu_copy_;
    IAllocatorUniquePtr<T> cpu_pinned_copy_;
    size_t count_;
    const CudaKernel* op_kernel_;
  };

  inline cublasHandle_t DefaultCublasHandle() const {
    return provider_->PerThreadDefaultCublasHandle();
  }

  inline cublasLtHandle_t CublasLtHandle() const {
    return provider_->PerThreadCublasLtHandle();
  }

  inline cudnnHandle_t DefaultCudnnHandle() const {
    return provider_->PerThreadDefaultCudnnHandle();
  }

 protected:
  template <typename T>
  inline const T* GetConstOnes(size_t count, cudaStream_t stream) const {
    return provider_->template GetConstOnes<T>(count, stream);
  }

  inline Status CopyTensor(const Tensor& src, Tensor& dst, onnxruntime::Stream* stream) const {
    auto* gpu_data_transfer = Info().GetDataTransferManager().GetDataTransfer(src.Location().device, dst.Location().device);
    return gpu_data_transfer->CopyTensorAsync(src, dst, stream);
  }

  inline int GetDeviceId() const { return provider_->GetDeviceId(); }

  static cudaStream_t GetCudaStreamFromContext(OpKernelContext* context) {
    auto* stream = context->GetComputeStream();
    return stream ? static_cast<cudaStream_t>(stream->handle) : nullptr;
  }

 private:
  CUDAExecutionProvider* provider_;
};

}  // namespace cuda
}  // namespace onnxruntime
