#include <ATen/ATen.h>
#include <ATen/Dispatch.h>
#include <ATen/native/TensorIterator.h>
#include <ATen/native/UpSample.h>
#include <ATen/native/cpu/Loops.h>
#include <ATen/native/quantized/cpu/quantized_ops.h>
#include <ATen/quantized/Quantizer.h>
#include <ATen/native/SortingUtils.h>

#include <cmath>
#ifdef USE_FBGEMM
#include <fbgemm/QuantUtils.h>
#endif
#ifdef _OPENMP
#include <omp.h>
#endif

namespace at {
namespace native {
namespace {

// ****************** HEY YOU! YES YOU! Read this! ********************
//
// Please read the README.md in this directory before editing this file

template <bool ReLUFused = false>
Tensor qcat_nhwc_kernel(
    const c10::List<Tensor>& qxs,
    int64_t dim,
    double scale,
    int64_t zero_point) {
  const at::Tensor& qx0 = qxs[0];
  int64_t C_out = 0;
  std::vector<int64_t> Cs_in;
  // Prefix sum of input channels for fast indexing
  std::vector<int64_t> Cs_sum;
  std::vector<double> scales;
  std::vector<int64_t> zero_pts;
  std::vector<void*> data_ptrs;

  for (const at::Tensor& qx : qxs) {
    TORCH_CHECK(
        qx.dim() == qx0.dim(),
        "Tensors must have the same number of dimensions: got ",
        qx.dim(),
        " and ",
        qx0.dim());
#define CHECK_DIM(d)                                            \
  TORCH_CHECK(                                                  \
      qx.size(d) == qx0.size(d),                                \
      "Sizes of tensors must match expect in dimension 1. Got", \
      qx.size(d),                                               \
      " and ",                                                  \
      qx0.size(d));
    CHECK_DIM(0);
    CHECK_DIM(2);
    CHECK_DIM(3);
    TORCH_CHECK(
        qx.scalar_type() == qx0.scalar_type(),
        "Expected object of scalar type ",
        toString(qx0.scalar_type()),
        " but got scalar type ",
        toString(qx.scalar_type()));
    Cs_in.push_back(qx.size(1));
    Cs_sum.push_back(C_out);
    C_out += qx.size(1);
    scales.push_back(qx.q_scale());
    zero_pts.push_back(qx.q_zero_point());
    data_ptrs.push_back(qx.data_ptr());
  }

  const int64_t N = qx0.size(0);
  const int64_t H = qx0.size(2);
  const int64_t W = qx0.size(3);
  float inv_scale = 1.0 / scale;

  auto output = at::_empty_affine_quantized(
      {N, C_out, H, W},
      qx0.options().memory_format(MemoryFormat::ChannelsLast),
      scale,
      zero_point,
      c10::nullopt);

  // N, H, and W are explicitly captured here because there's a bug in GCC5
  // which causes an internal compiler error if they're not
  AT_DISPATCH_QINT_TYPES(output.scalar_type(), "qcat_nhwc", [&, N, H, W]() {
    using Vec = Vec256<scalar_t>;
    for (int64_t batch = 0; batch < N; ++batch) {
      for (int64_t row = 0; row < H; ++row) {
        for (int64_t col = 0; col < W; ++col) {
          // loop over input tensors
          for (int64_t tidx = 0; tidx < Cs_in.size(); ++tidx) {
            scalar_t::underlying* optr =
                reinterpret_cast<scalar_t::underlying*>(output.data_ptr()) +
                batch * H * W * C_out + row * W * C_out + col * C_out +
                Cs_sum[tidx];

            auto curr_C = Cs_in[tidx];
            float curr_scale = scales[tidx];
            int64_t curr_zero_pt = zero_pts[tidx];

            scalar_t::underlying* iptr =
                reinterpret_cast<scalar_t::underlying*>(data_ptrs[tidx]) +
                batch * H * W * curr_C + row * W * curr_C + col * curr_C;

            constexpr int64_t VLEN = Vec::size();
            int64_t c = 0;

            // Vectorized loop
            if (c + VLEN <= curr_C) {
              auto curr_scale_vec = Vec256<float>(curr_scale);
              auto curr_zero_pt_vec = Vec256<float>((float)curr_zero_pt);
              auto scale_neg_zp_premul = curr_scale_vec * curr_zero_pt_vec.neg();
              for (; c + VLEN <= curr_C; c += VLEN) {
                auto inp_vec = Vec::loadu(iptr + c);
                auto float_values = inp_vec.dequantize(
                    curr_scale_vec, curr_zero_pt_vec, scale_neg_zp_premul);
                Vec::float_vec_return_type retvals;
                for (int i = 0; i < Vec::float_num_vecs(); ++i) {
                  if (ReLUFused) {
                    retvals[i] =
                        vec256::maximum(float_values[i], Vec256<float>(0.0f));
                  } else {
                    retvals[i] = float_values[i];
                  }
                }
                auto quantized =
                    Vec::quantize(retvals, scale, zero_point, inv_scale);
                quantized.store(optr + c);
              }
            }

            // Scalar loop
            for (; c < curr_C; ++c) {
              auto float_val = at::dequantize_val(
                  curr_scale,
                  curr_zero_pt,
                  reinterpret_cast<scalar_t*>(iptr)[c]);
              if (ReLUFused) {
                float_val = std::max(0.0f, float_val);
              }
              optr[c] =
                  at::quantize_val<scalar_t>(scale, zero_point, float_val).val_;
            } // for c

          } // for tidx
        } // for col
      } // for row
    } // for b
  });

  return output;
}

// horizontal sum over a range of uint8_t
int64_t hsum(const uint8_t* A, int len) {
  int64_t row_sum = 0;
  int i = 0;

#ifdef __AVX2__
  __m256i sum_v = _mm256_setzero_si256();
  __m256i one_epi16_v = _mm256_set1_epi16(1);
  __m256i one_epi8_v = _mm256_set1_epi8(1);
  // vectorized
  for (; i < len / 32 * 32; i += 32) {
    __m256i src_v = _mm256_loadu_si256(reinterpret_cast<__m256i const*>(A + i));
    sum_v = _mm256_add_epi32(
      sum_v,
      _mm256_madd_epi16(
        // first argument is unsigned, second is signed
        _mm256_maddubs_epi16(src_v, one_epi8_v),
      one_epi16_v)
    );
  }

  alignas(64) int32_t temp[8];
  _mm256_store_si256(reinterpret_cast<__m256i*>(temp), sum_v);
  for (int k = 0; k < 8; ++k) {
    row_sum += temp[k];
  }
#endif // __AVX2__

  // scalar
  for (; i < len; ++i) {
    row_sum += A[i];
  }

  return row_sum;
}

// horizontal sum over a range of int8_t
int64_t hsum(const int8_t* A, int len) {
  int64_t row_sum = 0;
  int i = 0;

#ifdef __AVX2__
  __m256i sum_v = _mm256_setzero_si256();
  __m256i one_epi16_v = _mm256_set1_epi16(1);
  __m256i one_epi8_v = _mm256_set1_epi8(1);
  // vectorized
  for (; i < len / 32 * 32; i += 32) {
    __m256i src_v = _mm256_loadu_si256(reinterpret_cast<__m256i const*>(A + i));
    sum_v = _mm256_add_epi32(
      sum_v,
      _mm256_madd_epi16(
        // first argument is unsigned, second is signed
        _mm256_maddubs_epi16(one_epi8_v, src_v),
      one_epi16_v)
    );
  }

  alignas(64) int32_t temp[8];
  _mm256_store_si256(reinterpret_cast<__m256i*>(temp), sum_v);
  for (int k = 0; k < 8; ++k) {
    row_sum += temp[k];
  }
#endif // __AVX2__

  // scalar
  for (; i < len; ++i) {
    row_sum += A[i];
  }

  return row_sum;
}

// horizontal sum over a range of int32_t
int64_t hsum(const int32_t* A, int len) {
  int64_t row_sum = 0;
  int i = 0;

#ifdef __AVX2__
  __m256i sum_epi64 = _mm256_setzero_si256();
  // vectorized
  for (; i < len / 8 * 8; i += 8) {
    __m256i src_epi32 = _mm256_loadu_si256(reinterpret_cast<__m256i const*>(A + i));
    // widen
    __m128i src_lo_epi32 = _mm256_castsi256_si128(src_epi32);
    __m128i src_hi_epi32 = _mm256_extractf128_si256(src_epi32, 1);
    __m256i src_lo_epi64 = _mm256_cvtepi32_epi64(src_lo_epi32);
    __m256i src_hi_epi64 = _mm256_cvtepi32_epi64(src_hi_epi32);
    // add
    sum_epi64 = _mm256_add_epi64(sum_epi64, src_lo_epi64);
    sum_epi64 = _mm256_add_epi64(sum_epi64, src_hi_epi64);
  }

  alignas(64) int64_t temp[4];
  _mm256_store_si256(reinterpret_cast<__m256i*>(temp), sum_epi64);
  for (int k = 0; k < 4; ++k) {
    row_sum += temp[k];
  }
#endif // __AVX2__

  // scalar
  for (; i < len; ++i) {
    row_sum += A[i];
  }

  return row_sum;
}

// horizontal sum of squares over a range of uint8_t
int64_t hsum_sq(const uint8_t* A, int len) {
  int64_t row_sum = 0;
  int i = 0;

#ifdef __AVX2__
  __m256i sum_v_epu32 = _mm256_setzero_si256();
  // vectorized
  for (; i < len / 16 * 16; i += 16) {
    // (i15, ..., i0)
    __m128i src_epu8 = _mm_loadu_si128(reinterpret_cast<__m128i const*>(A + i));
    __m256i src_epu16 = _mm256_cvtepu8_epi16(src_epu8);
    // (i15 ^ 2, ..., i0 ^ 2)
    __m256i sq_epu16 = _mm256_mullo_epi16(src_epu16, src_epu16);
    // (i7 ^ 2, ..., i0 ^ 2)
    __m128i sq_lo_epu16 = _mm256_castsi256_si128(sq_epu16);
    // (i15 ^ 2, ..., i8 ^ 2)
    __m128i sq_hi_epu16 = _mm256_extractf128_si256(sq_epu16, 1);
    // widen to epu32
    __m256i sq_lo_epu32 = _mm256_cvtepu16_epi32(sq_lo_epu16);
    __m256i sq_hi_epu32 = _mm256_cvtepu16_epi32(sq_hi_epu16);
    // add to running sum
    sum_v_epu32 = _mm256_add_epi32(sum_v_epu32, sq_lo_epu32);
    sum_v_epu32 = _mm256_add_epi32(sum_v_epu32, sq_hi_epu32);
  }

  alignas(64) int32_t temp[8];
  _mm256_store_si256(reinterpret_cast<__m256i*>(temp), sum_v_epu32);
  for (int k = 0; k < 8; ++k) {
    row_sum += temp[k];
  }
#endif // __AVX2__

  // scalar
  for (; i < len; ++i) {
    row_sum += A[i] * A[i];
  }

  return row_sum;
}

// horizontal sum of squares over a range of int8_t
int64_t hsum_sq(const int8_t* A, int len) {
  int64_t row_sum = 0;
  int i = 0;

#ifdef __AVX2__
  __m256i sum_v_epi32 = _mm256_setzero_si256();
  // vectorized
  for (; i < len / 16 * 16; i += 16) {
    // (i15, ..., i0)
    __m128i src_epi8 = _mm_loadu_si128(reinterpret_cast<__m128i const*>(A + i));
    __m256i src_epi16 = _mm256_cvtepi8_epi16(src_epi8);
    // (i15 ^ 2, ..., i0 ^ 2)
    __m256i sq_epi16 = _mm256_mullo_epi16(src_epi16, src_epi16);
    // (i7 ^ 2, ..., i0 ^ 2)
    __m128i sq_lo_epi16 = _mm256_castsi256_si128(sq_epi16);
    // (i15 ^ 2, ..., i8 ^ 2)
    __m128i sq_hi_epi16 = _mm256_extractf128_si256(sq_epi16, 1);
    // widen to epi32
    __m256i sq_lo_epi32 = _mm256_cvtepi16_epi32(sq_lo_epi16);
    __m256i sq_hi_epi32 = _mm256_cvtepi16_epi32(sq_hi_epi16);
    // add to running sum
    sum_v_epi32 = _mm256_add_epi32(sum_v_epi32, sq_lo_epi32);
    sum_v_epi32 = _mm256_add_epi32(sum_v_epi32, sq_hi_epi32);
  }

  alignas(64) int32_t temp[8];
  _mm256_store_si256(reinterpret_cast<__m256i*>(temp), sum_v_epi32);
  for (int k = 0; k < 8; ++k) {
    row_sum += temp[k];
  }
#endif // __AVX2__

  // scalar
  for (; i < len; ++i) {
    row_sum += A[i] * A[i];
  }

  return row_sum;
}

// horizontal sum os squares over a range of int32_t
// floats throughout are necessary to prevent overflow
float hsum_sq(const int32_t* A, int len) {
  float row_sum = 0;
  int i = 0;

#ifdef __AVX2__
  __m256 sum_ps = _mm256_setzero_ps();
  // vectorized
  for (; i < len / 8 * 8; i += 8) {
    __m256i src_epi32 = _mm256_loadu_si256(reinterpret_cast<__m256i const*>(A + i));
    __m256 src_ps = _mm256_cvtepi32_ps(src_epi32);
    sum_ps = _mm256_add_ps(sum_ps, _mm256_mul_ps(src_ps, src_ps));
  }

  alignas(64) float temp[8];
  _mm256_store_ps(temp, sum_ps);
  for (int k = 0; k < 8; ++k) {
    row_sum += static_cast<float>(temp[k]);
  }
#endif // __AVX2__

  // scalar
  for (; i < len; ++i) {
    int64_t cur = static_cast<int64_t>(A[i]);
    row_sum += (float)cur * (float)cur;
  }

  return row_sum;
}

void qrelu_kernel(const Tensor& qx, Tensor& qy) {
  const auto zero_point = qx.q_zero_point();
  AT_DISPATCH_QINT_TYPES(qx.scalar_type(), "qrelu", [&]() {
    qy = at::_empty_affine_quantized(
        qx.sizes(),
        at::device(kCPU).dtype(SCALAR_TYPE).memory_format(qx.suggest_memory_format()),
        qx.q_scale(),
        qx.q_zero_point(),
        c10::nullopt);
    using Vec = Vec256<scalar_t>;
    auto zero_point_vec = Vec(scalar_t(zero_point));
    auto iter = TensorIterator::unary_op(qy, qx);
    cpu_kernel_vec(
        iter,
        [&](scalar_t value) -> scalar_t {
          return scalar_t(std::max<underlying_t>(value.val_, zero_point));
        },
        [&](Vec value) -> Vec { return value.relu(zero_point_vec); });
  });
}

void qrelu6_kernel(const Tensor& qx, Tensor& qy) {
  const auto zero_point = qx.q_zero_point();
  AT_DISPATCH_QINT_TYPES(qx.scalar_type(), "qrelu6", [&]() {
    qy = at::_empty_affine_quantized(
        qx.sizes(),
        at::device(kCPU).dtype(SCALAR_TYPE).memory_format(qx.suggest_memory_format()),
        qx.q_scale(),
        qx.q_zero_point(),
        c10::nullopt);
    using Vec = Vec256<scalar_t>;
    auto iter = TensorIterator::unary_op(qy, qx);
    scalar_t six =
        at::quantize_val<scalar_t>(qx.q_scale(), qx.q_zero_point(), 6.0);
    auto zero_point_vec = Vec(scalar_t(zero_point));
    auto six_vec = Vec(six);
    cpu_kernel_vec(
        iter,
        [&](scalar_t value) -> scalar_t {
          underlying_t relu_val =
              std::max<underlying_t>(value.val_, zero_point);
          return scalar_t(std::min<underlying_t>(relu_val, six.val_));
        },
        [&](Vec val) -> Vec { return val.relu6(zero_point_vec, six_vec); });
  });
}

static void leaky_qrelu_out_kernel(Tensor& out, const Tensor& qx,
                                   Scalar negval_) {
  int64_t i_zp = qx.q_zero_point();
  float i_scale = qx.q_scale();

  int64_t o_zp = out.q_zero_point();
  float o_scale = out.q_scale();
  float o_inv_scale = 1.0f / o_scale;

  float negval = negval_.to<float>();

  AT_DISPATCH_QINT_TYPES(out.scalar_type(), "leaky_qrelu", [&] {
    using Vec = Vec256<float>;  // Naive implementation uses dequant/quant loop.
    using qVec = Vec256<scalar_t>;
    Vec zero_vec = Vec(0.0f);
    Vec one_vec = Vec(1.0f);

    Vec i_scale_vec = Vec((float)i_scale);
    Vec i_zp_vec = Vec((float)i_zp);
    Vec i_scale_zp_neg_premul_vec = i_scale_vec * i_zp_vec.neg();

    Vec negval_vec = Vec(negval);

    auto iter = TensorIterator::unary_op(out, qx);

    cpu_kernel_vec(
        iter,
        [&](scalar_t value_qx) -> scalar_t {
          auto value_dx = at::dequantize_val(i_scale, i_zp, value_qx);
          auto value_dy = value_dx > 0 ? value_dx : value_dx * negval;
          return at::quantize_val<scalar_t>(o_scale, o_zp, value_dy);
        },
        [&](qVec qx_vec) -> qVec {
          /* Vectorized implementation creates a multiplicand vector, which has
           * "alpha" for all negative dx values and ones-vector for all
           * positive values of dx. The multiplicand then is multiplied by the
           * input.
           */
          auto dx_vec_vec = qx_vec.dequantize(i_scale_vec, i_zp_vec,
                                              i_scale_zp_neg_premul_vec);
          for (int idx = 0; idx < dx_vec_vec.size(); ++idx) {
            const auto dx_vec = dx_vec_vec[idx];
            const auto multiplicand = Vec::blendv(negval_vec, one_vec,
                                                  dx_vec > zero_vec);
            dx_vec_vec[idx] = dx_vec * multiplicand;
          }
          return qVec::quantize(dx_vec_vec, o_scale, o_zp, o_inv_scale);
        });
  });
}

void qsigmoid_kernel(const Tensor& qx, Tensor& qy) {
  int64_t zero_point = qx.q_zero_point();
  float scale = qx.q_scale();
  auto scale_vec = Vec256<float>(scale);
  auto zero_point_vec = Vec256<float>((float)zero_point);
  auto scale_neg_zp_premul_vec = scale_vec * zero_point_vec.neg();

  AT_DISPATCH_QINT_TYPES(qx.scalar_type(), "qsigmoid", [&]() {
    // Naive implemenentation: uses dequantize/execute/quantize routine
    // - Output scale is set to 1.0 / 2^(BIT_NUM)
    // - For signed types output zero point is set to 0
    // - For unsigned types output zero point is set to (qmax + qmin) / 2.0
    // See https://stackoverflow.com/a/34448562/3606192 for potential
    // optimizations
    float output_scale = 0.00390625;  // 1.0 / 2^8
    int64_t output_zero_point = 0;
    if (SCALAR_TYPE == at::kQInt32) {
      output_scale = 2.3283064365386963e-10;  // 1.0 / 2^32
    } else if (SCALAR_TYPE == at::kQInt8) {
      output_zero_point = -128;
    }
    float inv_output_scale = 1.0 / output_scale;

    qy = at::_empty_affine_quantized(
        qx.sizes(),
        at::device(kCPU).dtype(SCALAR_TYPE).memory_format(qx.suggest_memory_format()),
        output_scale,
        output_zero_point,
        c10::nullopt);
    auto iter = TensorIterator::unary_op(qy, qx);

    using Vec = Vec256<scalar_t>;
    cpu_kernel_vec(
      iter,
      [&](scalar_t value_qx) -> scalar_t {
        const auto value_dx = at::dequantize_val(scale, zero_point, value_qx);
        const auto value_dy = 1.0f / (1.0 + std::exp((-value_dx)));
        return at::quantize_val<scalar_t>(output_scale, output_zero_point,
                                          value_dy);
      },
      [&](Vec value_qx) -> Vec {
        auto value_dx = value_qx.dequantize(scale_vec, zero_point_vec,
                                            scale_neg_zp_premul_vec);
        for (int idx = 0; idx < value_dx.size(); ++idx) {
          value_dx[idx] = value_dx[idx].neg();
          value_dx[idx] = value_dx[idx].exp();
          value_dx[idx] = Vec256<float>(1.0f) + value_dx[idx];
          value_dx[idx] = value_dx[idx].reciprocal();
        }
        return Vec::quantize(value_dx, output_scale, output_zero_point,
                             inv_output_scale);
      }
    );
  });
}

void qhardsigmoid_kernel(const Tensor& qx, Tensor& qy) {
  int64_t zero_point = qx.q_zero_point();
  float scale = qx.q_scale();
  auto scale_vec = Vec256<float>(scale);
  auto zero_point_vec = Vec256<float>((float)zero_point);
  auto scale_neg_zp_premul_vec = scale_vec * zero_point_vec.neg();

  AT_DISPATCH_QINT_TYPES(qx.scalar_type(), "qhardsigmoid", [&]() {

    // - Output scale is set to 1.0 / 2^(BIT_NUM)
    float output_scale = 0.00390625;  // 1.0 / 2^8
    if (SCALAR_TYPE == at::kQInt32) {
      output_scale = 2.3283064365386963e-10;  // 1.0 / 2^32
    }
    float inv_output_scale = 1.0 / output_scale;

    // The default zero-point is zero.  As a one-off optimization for
    // kQInt8, we set the zero-point to -128 to maximize precision in the
    // [0, 1] output range. kQInt32 can be handled in a future PR if needed.
    int64_t output_zero_point = 0;
    if (SCALAR_TYPE == at::kQInt8) {
      output_zero_point = -128;
    }

    qy = at::_empty_affine_quantized(
        qx.sizes(),
        at::device(kCPU).dtype(SCALAR_TYPE),
        output_scale,
        output_zero_point,
        qx.suggest_memory_format());
    auto iter = TensorIterator::unary_op(qy, qx);

    using qVec = Vec256<scalar_t>;
    using fVec = Vec256<float>;
    fVec kZeroVec(0.0f);
    fVec kThreeVec(3.0f);
    fVec kSixVec(6.0f);

    // Naive implemenentation: uses dequantize/execute/quantize routine
    cpu_kernel_vec(
      iter,
      [&](scalar_t qx) -> scalar_t {
        auto x = at::dequantize_val(scale, zero_point, qx);
        const auto y = std::min(std::max(x + 3.0f, 0.0f), 6.0f) / 6.0f;
        return at::quantize_val<scalar_t>(output_scale, output_zero_point, y);
      },
      [&](qVec value_qx) -> qVec {
        auto value_dx = value_qx.dequantize(scale_vec, zero_point_vec,
                                            scale_neg_zp_premul_vec);
        for (int idx = 0; idx < value_dx.size(); ++idx) {
          value_dx[idx] = vec256::minimum(
            vec256::maximum(value_dx[idx] + kThreeVec, kZeroVec),
            kSixVec
          ) / kSixVec;
        }
        return qVec::quantize(value_dx, output_scale, output_zero_point,
                             inv_output_scale);
      }
    );
  });
}

void qclamp_kernel(
    const Tensor& qx,
    Scalar min_scalar,
    Scalar max_scalar,
    Tensor& qy) {
  AT_DISPATCH_QINT_TYPES(qx.scalar_type(), "qclamp", [&]() {
    qy = at::_empty_affine_quantized(
        qx.sizes(),
        at::device(kCPU).dtype(SCALAR_TYPE).memory_format(qx.suggest_memory_format()),
        qx.q_scale(),
        qx.q_zero_point(),
        c10::nullopt);
    using Vec = Vec256<scalar_t>;
    auto iter = TensorIterator::unary_op(qy, qx);
    auto min = min_scalar.to<float>();
    auto max = max_scalar.to<float>();
    scalar_t min_q =
        at::quantize_val<scalar_t>(qx.q_scale(), qx.q_zero_point(), min);
    scalar_t max_q =
        at::quantize_val<scalar_t>(qx.q_scale(), qx.q_zero_point(), max);
    auto min_vec = Vec(min_q);
    auto max_vec = Vec(max_q);
    cpu_kernel_vec(
        iter,
        [&](scalar_t value) -> scalar_t {
          underlying_t min_clamped =
              std::max<underlying_t>(value.val_, min_q.val_);
          return scalar_t(std::min<underlying_t>(min_clamped, max_q.val_));
        },
        [&](Vec val) -> Vec {
          auto min_clamped = val.maximum(min_vec);
          return min_clamped.minimum(max_vec);
        });
  });
}

void qhardswish_kernel(const Tensor& qx, Tensor& qy) {
  const auto i_scale = qx.q_scale();
  const auto i_zero_point = qx.q_zero_point();

  const auto o_scale = qy.q_scale();
  const auto o_zero_point = qy.q_zero_point();
  const float o_inv_scale = 1.0 / o_scale;

  using fVec = Vec256<float>;
  fVec i_scale_vec(i_scale);
  fVec i_zero_point_vec(i_zero_point);
  fVec i_scale_neg_zp_premul_vec = i_scale_vec * i_zero_point_vec.neg();
  fVec zero_vec(0.0f);
  fVec three_vec(3.0f);
  fVec six_vec(6.0f);

  AT_DISPATCH_QINT_TYPES(qx.scalar_type(), "qhardswish", [&]() {
    using qVec = Vec256<scalar_t>;
    auto iter = TensorIterator::unary_op(qy, qx);
    cpu_kernel_vec(
        iter,
        [&](scalar_t value) -> scalar_t {
          const auto x = at::dequantize_val(i_scale, i_zero_point, value);
          const auto y = x * std::min(std::max(x + 3.0f, 0.0f), 6.0f) / 6.0f;
          return at::quantize_val<scalar_t>(o_scale, o_zero_point, y);
        },
        [&](qVec value) -> qVec {
          auto value_dx = value.dequantize(i_scale_vec, i_zero_point_vec,
                                           i_scale_neg_zp_premul_vec);
          for (int idx = 0; idx < value_dx.size(); idx++) {
            value_dx[idx] = value_dx[idx] * vec256::minimum(
              vec256::maximum(value_dx[idx] + three_vec, zero_vec),
              six_vec
            ) / six_vec;
          }
          return qVec::quantize(value_dx, o_scale, o_zero_point, o_inv_scale);
        });
  });
}


void qtanh_kernel(const Tensor& qx, Tensor& qy) {
  int64_t zero_point = qx.q_zero_point();
  float scale = qx.q_scale();
  auto scale_vec = Vec256<float>(scale);
  auto zero_point_vec = Vec256<float>((float)zero_point);
  auto scale_neg_zp_premul_vec = scale_vec * zero_point_vec.neg();

  AT_DISPATCH_QINT_TYPES(qx.scalar_type(), "qtanh", [&]() {
    // Naive implemenentation: uses dequantize/execute/quantize routine
    // - Output scale is set to 2.0 / 2^(BIT_NUM)
    // - For signed types output zero point is set to 0
    // - For unsigned types output zero point is set to (qmax + qmin) / 2.0
    float output_scale = 0.0078125;  // 2.0 / 512
    int64_t output_zero_point = 0;
    if (SCALAR_TYPE == at::kQInt32) {
      output_scale = 4.656612873077393e-10;  // 2.0 / 2^32
    } else if (SCALAR_TYPE == at::kQUInt8) {
      output_zero_point = 128;
    }
    float inv_output_scale = 1.0 / output_scale;

    qy = at::_empty_affine_quantized(
        qx.sizes(),
        at::device(kCPU).dtype(SCALAR_TYPE).memory_format(qx.suggest_memory_format()),
        output_scale,
        output_zero_point,
        c10::nullopt);
    auto iter = TensorIterator::unary_op(qy, qx);

    using Vec = Vec256<scalar_t>;
    cpu_kernel_vec(
      iter,
      [&](scalar_t value_qx) -> scalar_t {
        const auto value_dx = at::dequantize_val(scale, zero_point, value_qx);
        return at::quantize_val<scalar_t>(output_scale, output_zero_point,
                                          std::tanh(value_dx));
      },
      [&](Vec value_qx) -> Vec {
        const auto value_dx = value_qx.dequantize(scale_vec, zero_point_vec,
                                                  scale_neg_zp_premul_vec);
        Vec::float_vec_return_type retvals;
        for (int idx = 0; idx < Vec::float_num_vecs(); ++idx) {
          retvals[idx] = value_dx[idx].tanh();
        }
        return Vec::quantize(retvals, output_scale, output_zero_point,
                             inv_output_scale);
      }
    );
  });
}

void qelu_kernel(const Tensor& qx, Scalar alpha, Tensor& qy) {

  int64_t i_zp = qx.q_zero_point();
  float i_scale = qx.q_scale();

  // In a future PR, we can improve on output scale and zero_point
  // selection.
  int64_t o_zp = qy.q_zero_point();
  float o_scale = qy.q_scale();
  float inv_o_scale = 1.0 / o_scale;

  float alpha_float = alpha.to<float>();

  AT_DISPATCH_QINT_TYPES(qx.scalar_type(), "qelu_kernel", [&] {

    auto iter = TensorIterator::unary_op(qy, qx);

    // vectorized
    using Vec = Vec256<float>;
    using qVec = Vec256<scalar_t>;

    Vec zero_vec = Vec(0.0f);
    Vec one_vec = Vec(1.0f);
    Vec alpha_vec = Vec(alpha_float);
    Vec i_scale_vec = Vec(i_scale);
    Vec i_zero_point_vec = Vec((float)i_zp);
    Vec i_scale_neg_zp_premul_vec = i_scale_vec * i_zero_point_vec.neg();

    cpu_kernel_vec(
      iter,
      [&](scalar_t value_qx) -> scalar_t {
        // dequantize
        const auto x = at::dequantize_val(i_scale, i_zp, value_qx);
        // ELU
        const auto y = x >= 0
          ? x
          : (alpha_float * (std::exp(x) - 1));
        // quantize
        return at::quantize_val<scalar_t>(o_scale, o_zp, y);
      },
      [&](qVec value_qx) -> qVec {
        // dequantize
        auto dx_vec_vec = value_qx.dequantize(i_scale_vec, i_zero_point_vec,
                                            i_scale_neg_zp_premul_vec);
        for (int idx = 0; idx < dx_vec_vec.size(); idx++) {

          // quickly check if any elements are below zero
          auto cmp_to_zero = dx_vec_vec[idx] > zero_vec;

          if (cmp_to_zero.zero_mask()) {

            Vec dx_vec_copy_neg_elu = dx_vec_vec[idx] * one_vec;
            // calculate the negative part of ELU on the copy
            dx_vec_copy_neg_elu = dx_vec_copy_neg_elu.exp();
            dx_vec_copy_neg_elu = dx_vec_copy_neg_elu - one_vec;
            dx_vec_copy_neg_elu = dx_vec_copy_neg_elu * alpha_vec;
            // blend
            dx_vec_vec[idx] = Vec::blendv(dx_vec_copy_neg_elu, dx_vec_vec[idx],
                                        dx_vec_vec[idx] > zero_vec);
          }
        }
        // quantize
        return qVec::quantize(dx_vec_vec, o_scale, o_zp, inv_o_scale);
      }
    );

  });
}

// Note: out is assumed to be the same size as self and other.
// Note: Addition is only supported when self and out are of the same dtype.
// Note: other is already assumed to be in int32, i.e., it's
// round(float/self_scale)
template <bool ReLUFused = false>
void qadd_scalar_kernel(Tensor& out, const Tensor& self, Scalar other) {
  int64_t zero_point = out.q_zero_point();
  float scale = out.q_scale();
  float inv_scale = 1.0f / scale;
  int64_t self_zero_point = self.q_zero_point();
  float self_scale = self.q_scale();

  float multiplier = self_scale * inv_scale;

  AT_DISPATCH_QINT_TYPES(self.scalar_type(), "qadd_scalar", [&]() {
    using Vec = Vec256<scalar_t>;
    auto iter = TensorIterator::unary_op(out, self);
    auto other_val = other.to<int32_t>();
    auto other_vec = Vec256<c10::qint32>(static_cast<c10::qint32>(other_val));
    cpu_kernel_vec(
        iter,
        [&](scalar_t a) -> scalar_t {
          int32_t a_sub_z = static_cast<int32_t>(a.val_) -
              static_cast<int32_t>(self_zero_point);
          int32_t c = a_sub_z + other_val;
          scalar_t res =
              at::requantize_from_int<scalar_t>(multiplier, zero_point, c);
          if (ReLUFused) {
            res.val_ = std::max<scalar_t::underlying>(res.val_, zero_point);
          }
          return res;
        },
        [&](Vec a) -> Vec {
          Vec::int_vec_return_type a_sub_z =
              a.widening_subtract(Vec(static_cast<scalar_t>(self_zero_point)));
          Vec::int_vec_return_type c;
          for (int i = 0; i < Vec::int_num_vecs(); ++i) {
            c[i] = a_sub_z[i] + other_vec;
          }
          Vec rv = Vec::requantize_from_int(c, multiplier, zero_point);
          if (ReLUFused) {
            rv = rv.maximum(Vec(static_cast<scalar_t>(zero_point)));
          }
          return rv;
        });
  });
}
// Note: out is assumed to be the same size as self and other.
// Note: Addition is only supported when self, other, out are of the same dtype.
template <bool ReLUFused = false>
void qadd_kernel(Tensor& out, const Tensor& self, const Tensor& other) {
  int64_t zero_point = out.q_zero_point();
  float scale = out.q_scale();
  float inv_scale = 1.0f / scale;
  int64_t self_zero_point = self.q_zero_point();
  float self_scale = self.q_scale();
  int64_t other_zero_point = other.q_zero_point();
  float other_scale = other.q_scale();

  // Broadcast out the parameters here to amortize out that cost across
  // loop iterations.
  // TODO: we can optimize dequantization by doing a premultiplication
  // of the zero point by scale and doing FMA on scale*x_q - (scale*zero_point)
  auto self_zero_point_vec = Vec256<float>((float)self_zero_point);
  auto self_scale_vec = Vec256<float>(self_scale);
  auto other_zero_point_vec = Vec256<float>((float)other_zero_point);
  auto other_scale_vec = Vec256<float>(other_scale);

  auto self_scale_neg_zp_premul_vec = self_scale_vec * self_zero_point_vec.neg();
  auto other_scale_zp_premul_vec = other_scale_vec * other_zero_point_vec.neg();

  auto iter = TensorIterator::binary_op(out, self, other);

  AT_DISPATCH_QINT_TYPES(out.scalar_type(), "qadd", [&]() {
    using Vec = Vec256<scalar_t>;
    cpu_kernel_vec(
        iter,
        [&](scalar_t a, scalar_t b) -> scalar_t {
          const auto da = at::dequantize_val(self_scale, self_zero_point, a);
          const auto db = at::dequantize_val(other_scale, other_zero_point, b);
          float c = da + db;
          if (ReLUFused) {
            c = std::max<float>(c, 0.0);
          }
          return at::quantize_val<scalar_t>(scale, zero_point, c);
        },
        [&](Vec a, Vec b) -> Vec {
          const auto da = a.dequantize(
              self_scale_vec, self_zero_point_vec, self_scale_neg_zp_premul_vec);
          const auto db = b.dequantize(
              other_scale_vec, other_zero_point_vec, other_scale_zp_premul_vec);
          Vec::float_vec_return_type retvals;
          for (int i = 0; i < Vec::float_num_vecs(); ++i) {
            auto c = da[i] + db[i];
            if (ReLUFused) {
              c = vec256::maximum(c, Vec256<float>(0.0f));
            }
            retvals[i] = c;
          }
          // TODO: fbgemm::Quantize doesn't support taking in the
          // pre-broadcasted parameters. We might be able to save some cycles by
          // enabling that in the API.
          // TODO: specialize fbgemm::Quantize for a single vector and make it
          // inlineable. This could help with interleaving as suggested by the
          // TensorIterator implementations
          auto rv = Vec::quantize(retvals, scale, zero_point, inv_scale);
          return rv;
        });
  });
}

// Note: out is assumed to be the same size as self and other.
// Note: Multiplication is only supported when self, other, out are of the same
// dtype.
template <bool ReLUFused = false>
void qmul_kernel(Tensor& out, const Tensor& self, const Tensor& other) {
  int64_t zero_point = out.q_zero_point();
  float scale = out.q_scale();
  float inv_scale = 1.0f / scale;
  int64_t self_zero_point = self.q_zero_point();
  float self_scale = self.q_scale();
  int64_t other_zero_point = other.q_zero_point();
  float other_scale = other.q_scale();

  float multiplier = self_scale * other_scale * inv_scale;

  auto iter = TensorIterator::binary_op(out, self, other);

  AT_DISPATCH_QINT_TYPES(out.scalar_type(), "qmul", [&]() {
    using Vec = Vec256<scalar_t>;
    cpu_kernel_vec(
        iter,
        [&](scalar_t a, scalar_t b) -> scalar_t {
          int32_t a_sub_z = static_cast<int32_t>(a.val_) -
              static_cast<int32_t>(self_zero_point);
          int32_t b_sub_z = static_cast<int32_t>(b.val_) -
              static_cast<int32_t>(other_zero_point);
          int32_t c = a_sub_z * b_sub_z;
          scalar_t res =
              at::requantize_from_int<scalar_t>(multiplier, zero_point, c);
          if (ReLUFused) {
            res.val_ = std::max<scalar_t::underlying>(res.val_, zero_point);
          }
          return res;
        },
        [&](Vec a, Vec b) -> Vec {
          Vec::int_vec_return_type a_sub_zp =
              a.widening_subtract(Vec(static_cast<scalar_t>(self_zero_point)));
          Vec::int_vec_return_type b_sub_zp =
              b.widening_subtract(Vec(static_cast<scalar_t>(other_zero_point)));
          Vec::int_vec_return_type c;
          for (int i = 0; i < Vec::int_num_vecs(); ++i) {
            c[i] = a_sub_zp[i] * b_sub_zp[i];
          }
          Vec rv = Vec::requantize_from_int(c, multiplier, zero_point);
          if (ReLUFused) {
            rv = rv.maximum(Vec(static_cast<scalar_t>(zero_point)));
          }
          return rv;
        });
  });
}

void qmaxpool_2d_nhwc_kernel(
    const Tensor& qx,
    int64_t iC, // input/output channels
    int64_t iH,
    int64_t iW, // input sizes
    int64_t oH,
    int64_t oW, // output sizes
    int64_t kH,
    int64_t kW, // kernel size
    int64_t sH,
    int64_t sW, // strides
    int64_t pH,
    int64_t pW, // padding
    int64_t dH,
    int64_t dW, // dilation
    Tensor& qy) {
  AT_DISPATCH_QINT_TYPES(qx.scalar_type(), "max_pool2d_nhwc", [&]() {
    scalar_t* idata = static_cast<scalar_t*>(qx.data_ptr());
    scalar_t* odata = static_cast<scalar_t*>(qy.data_ptr());

    // Loop over N
    for (int64_t b = 0; b < qx.size(0); ++b) {
      // Loop over H
      auto* i_p =
          reinterpret_cast<scalar_t::underlying*>(idata + b * iW * iH * iC);
      for (int64_t row = 0; row < oH; ++row) {
        // Loop over W
        for (int64_t col = 0; col < oW; ++col) {
          // Pointer to output data for this specific N,H,W position
          auto* o_p = reinterpret_cast<scalar_t::underlying*>(
              odata + b * oH * oW * iC + row * oW * iC + col * iC);

          // Loop over reduction block
          int64_t h_start = row * sH - pH;
          int64_t w_start = col * sW - pW;
          int64_t h_end = std::min(h_start + (kH - 1) * dH + 1, iH);
          int64_t w_end = std::min(w_start + (kW - 1) * dW + 1, iW);
          while (h_start < 0)
            h_start += dH;
          while (w_start < 0)
            w_start += dW;

          int64_t c = 0;

          // Interleaved vector loop 4x
          constexpr auto vec_width = Vec256<scalar_t>::size();
          for (; c + 4 * vec_width <= iC; c += 4 * vec_width) {
            Vec256<scalar_t> acc{
                scalar_t(std::numeric_limits<scalar_t::underlying>::lowest())};
            Vec256<scalar_t> accs[4] = {acc, acc, acc, acc};
            int64_t tcntr = 0;
            int64_t x, y;
            for (y = h_start; y < h_end; y += dH) {
              for (x = w_start; x < w_end; x += dW) {
                for (int i = 0; i < 4; ++i) {
                  tcntr = y * iW + x;
                  auto vals = Vec256<scalar_t>::loadu(
                      i_p + tcntr * iC + c + Vec256<scalar_t>::size() * i);
                  accs[i] = vec256::maximum(accs[i], vals);
                }
              } // for x
            } // for y
            for (int i = 0; i < 4; ++i) {
              accs[i].store(o_p + c + Vec256<scalar_t>::size() * i);
            }
          } // for c

          // Vector loop
          for (; c + vec_width <= iC; c += vec_width) {
            Vec256<scalar_t> acc{
                scalar_t(std::numeric_limits<scalar_t::underlying>::lowest())};
            int64_t tcntr = 0;
            int64_t x, y;
            for (y = h_start; y < h_end; y += dH) {
              for (x = w_start; x < w_end; x += dW) {
                tcntr = y * iW + x;
                auto vals = Vec256<scalar_t>::loadu(i_p + tcntr * iC + c);
                acc = vec256::maximum(acc, vals);
              } // for x
            } // for y
            acc.store(o_p + c);
          } // for c

          for (; c < iC; ++c) {
            auto max_val = std::numeric_limits<scalar_t::underlying>::lowest();
            int64_t tcntr = 0;
            int64_t x, y;
            for (y = h_start; y < h_end; y += dH) {
              for (x = w_start; x < w_end; x += dW) {
                tcntr = y * iW + x;
                auto val = *(i_p + tcntr * iC + c);
                max_val = std::max(max_val, val);
              } // for x
            } // for y

            o_p[c] = max_val;
          } // for c
        } // for col
      } // for row
    } // for b
  });
}

template <typename T>
void do_avg_pool_nhwc_on_AVX2(
    const typename T::underlying* i_p,
    typename T::underlying* o_p,
    int& c_start,
    int input_zero_point_m_size,
    int output_zero_point,
    float multiplier,
    int dstart,
    int dend,
    int hstart,
    int hend,
    int wstart,
    int wend,
    int dsize,
    int hsize,
    int wsize,
    int csize) {
#if defined(__AVX2__) && !defined(_MSC_VER)
  // buffer for channel accumulator, used to interchange channel-loop
  // to inner-most, so that memory access of the input tensor data is
  // continuous.
  constexpr int cb_size = 16;
  constexpr int vec_width = Vec256<T>::size() / 4;
  constexpr int cb_step = cb_size * vec_width;
  Vec256<int32_t> acc_buffer[cb_size];
  Vec256<float> acc_buffer_fp[cb_size];

  if (vec_width == 8) {
    for (int c = c_start; c < csize; c += cb_step) {
      int cend = std::min(cb_size, (csize - c) / vec_width);
      // initialize loop
      for (int ic = 0; ic < cend; ic++) {
        acc_buffer[ic] = Vec256<int32_t>(input_zero_point_m_size);
      }
      // compute loop
      for (int id = dstart; id < dend; id++) {
        for (int ih = hstart; ih < hend; ih++) {
          for (int iw = wstart; iw < wend; iw++) {
            const int i_idx =
                (id * wsize * hsize + ih * wsize + iw) *
                    csize +
                c;
            for (int ic = 0; ic < cend; ic++) {
              auto vals = vec256::convert_to_int32<typename T::underlying>(
                  i_p + i_idx + ic * vec_width);
              acc_buffer[ic] = acc_buffer[ic] + vals;
            }
          }
        }
      }
      // convert int32 accumulative to fp32
      vec256::convert((int*)acc_buffer, (float*)acc_buffer_fp, cend * vec_width);

      // first quantize using AVX using 32 lanes, then 8, finally falls
      // back to single
      QuantizeAvx2<T>(
          (float*)acc_buffer_fp,
          o_p + c,
          cend * vec_width,
          multiplier,
          output_zero_point);
    }
    c_start = csize / vec_width * vec_width;
  }
#endif
}

template <typename T>
void do_avg_pool_on_AVX2(
    typename T::underlying* i_p,
    typename T::underlying* o_p,
    int64_t& c,
    int64_t channel_size,
    int64_t channel_multiplier,
    int32_t input_zero_point_m_size,
    int32_t output_zero_point,
    float multiplier,
    int64_t dstart,
    int64_t dend,
    int64_t hstart,
    int64_t hend,
    int64_t wstart,
    int64_t wend,
    int64_t stride_C,
    int64_t stride_D,
    int64_t stride_H,
    int64_t stride_W) {
#if defined(__AVX2__) && !defined(_MSC_VER)
  constexpr auto vec_width = Vec256<T>::size() / 4;
  if (vec_width == 8) {
    for (; c + vec_width <= channel_size; c += vec_width) {
      int64_t tcntr = 0;

      Vec256<int32_t> acc(input_zero_point_m_size);
      for (int64_t id = dstart; id < dend; id++) {
        for (int64_t ih = hstart; ih < hend; ih++) {
          for (int64_t iw = wstart; iw < wend; iw++) {
            tcntr = id * stride_D + ih * stride_H + iw * stride_W;
            auto vals = vec256::convert_to_int32<typename T::underlying>(
                i_p + tcntr * channel_multiplier + c * stride_C);
            acc = acc + vals;
          }
        }
      }
      int32_t acc_int[vec_width];
      float acc_fp[vec_width];
      acc.store(acc_int);
      vec256::convert(acc_int, acc_fp, vec_width);
      at::quantize_vec<T>(
          1.0f / multiplier,
          output_zero_point,
          acc_fp,
          reinterpret_cast<T*>(o_p + c),
          vec_width);
    }
  }
#endif
}

void qadaptive_avg_pool2d_nhwc_kernel(
    const Tensor& qx,
    Tensor& qy,
    int64_t b,
    int64_t sizeC,
    int64_t isizeH,
    int64_t isizeW,
    int64_t osizeH,
    int64_t osizeW,
    int64_t istrideB,
    int64_t istrideC,
    int64_t istrideH,
    int64_t istrideW) {
  AT_DISPATCH_QINT_TYPES(qx.scalar_type(), "adaptive_avg_pool2d_nhwc", [&]() {
    scalar_t* idata = static_cast<scalar_t*>(qx.data_ptr());
    scalar_t* odata = static_cast<scalar_t*>(qy.data_ptr());
    auto minimum = std::numeric_limits<scalar_t::underlying>::lowest();
    auto maximum = std::numeric_limits<scalar_t::underlying>::max();
    auto* i_p =
        reinterpret_cast<typename scalar_t::underlying*>(idata + b * istrideB);

    float input_scale = qx.q_scale();
    float output_scale = qy.q_scale();
    int input_zero_point = qx.q_zero_point();
    int output_zero_point = qy.q_zero_point();

    for (int64_t oh = 0; oh < osizeH; oh++) {
      int istartH = (int)std::floor((float)(oh * isizeH) / osizeH);
      int iendH = (int)std::ceil((float)((oh + 1) * isizeH) / osizeH);
      int kH = iendH - istartH;
      for (int64_t ow = 0; ow < osizeW; ow++) {
        auto* o_p = reinterpret_cast<typename scalar_t::underlying*>(
            odata + b * osizeH * osizeW * sizeC + (oh * osizeW + ow) * sizeC);
        int istartW = (int)std::floor((float)(ow * isizeW) / osizeW);
        int iendW = (int)std::ceil((float)((ow + 1) * isizeW) / osizeW);
        int kW = iendW - istartW;
        int size = kH * kW;
        float multiplier = input_scale / output_scale / size;
        int input_zero_point_m_size = -input_zero_point * size;
        int64_t c = 0;
        // For int8 or uint8quantization, we implicitly use int32 as
        // accumulation Or else, it will go to the slow path
        // TODO: support 16bit, 32bit, and etc.
        auto* internal_i_p = i_p + istartH * istrideH + istartW * istrideW;

        // TODO: more vectorization with loop interleaving
        do_avg_pool_on_AVX2<scalar_t>(
            internal_i_p,
            o_p,
            c,
            sizeC,
            1,
            input_zero_point_m_size,
            output_zero_point,
            multiplier,
            0,
            1,
            0,
            kH,
            0,
            kW,
            istrideC,
            1,
            istrideH,
            istrideW);
        // 1) The following loop handles the remaining channels
        // 2) It also handles the Non-AVX2 path
        for (; c < sizeC; ++c) {
          int32_t acc_int32 = input_zero_point_m_size;
          int64_t tcntr = 0;
          for (int64_t ih = 0; ih < kH; ih++) {
            for (int64_t iw = 0; iw < kW; iw++) {
              tcntr = ih * istrideH + iw * istrideW;
              auto val = *(internal_i_p + tcntr + c * istrideC);
              acc_int32 += val;
            }
          }
          // clamp
          o_p[c] = at::quantize_val<scalar_t>(
                       1.0f / multiplier, output_zero_point, acc_int32)
                       .val_;
        } // c
      } // oh
    } // ow
  });
}

void qavg_pool2d_nhwc_kernel(
    const Tensor& qx,
    Tensor& qy,
    int64_t b,
    int64_t nInputPlane,
    int64_t inputWidth,
    int64_t inputHeight,
    int64_t outputWidth,
    int64_t outputHeight,
    int kW,
    int kH,
    int dW,
    int dH,
    int padW,
    int padH,
    bool count_include_pad,
    c10::optional<int64_t> divisor_override) {
  AT_DISPATCH_QINT_TYPES(qx.scalar_type(), "avg_pool2d_nhwc", [&]() {
    scalar_t* idata = static_cast<scalar_t*>(qx.data_ptr());
    scalar_t* odata = static_cast<scalar_t*>(qy.data_ptr());
    int64_t batch_size = nInputPlane * inputWidth * inputHeight;
    auto* i_p = reinterpret_cast<typename scalar_t::underlying*>(
        idata + b * batch_size);

    // lift these operations outside the loop to reduce access overheads
    float input_scale = qx.q_scale();
    float output_scale = qy.q_scale();
    int input_zero_point = qx.q_zero_point();
    int output_zero_point = qy.q_zero_point();
    int64_t divisor_override_factor =
        divisor_override.has_value() ? divisor_override.value() : 0;

    for (int64_t oh = 0; oh < outputHeight; oh++) {
      for (int64_t ow = 0; ow < outputWidth; ow++) {
        auto* o_p = reinterpret_cast<typename scalar_t::underlying*>(
            odata + b * nInputPlane * outputWidth * outputHeight +
            (oh * outputWidth + ow) * nInputPlane);
        int64_t hstart = oh * dH - padH;
        int64_t wstart = ow * dW - padW;
        int64_t hend = std::min(hstart + kH, inputHeight + padH);
        int64_t wend = std::min(wstart + kW, inputWidth + padW);
        int64_t pool_size = (hend - hstart) * (wend - wstart);
        hstart = std::max(hstart, (int64_t)0);
        wstart = std::max(wstart, (int64_t)0);
        hend = std::min(hend, inputHeight);
        wend = std::min(wend, inputWidth);

        int size = (hend - hstart) * (wend - wstart);
        int divide_size = count_include_pad ? pool_size : size;
        int divide_factor =
            divisor_override_factor ? divisor_override_factor : divide_size;
        float multiplier = input_scale / output_scale / divide_factor;
        int input_zero_point_m_size = -input_zero_point * size;

        int64_t c = 0;
        // For int8 quantization, we implicitly use int32 as accumulation
        // Or else, it will go to the slow path
        // TODO: support 16bit, 32bit, and etc.
        do_avg_pool_on_AVX2<scalar_t>(
            i_p,
            o_p,
            c,
            nInputPlane,
            nInputPlane,
            input_zero_point_m_size,
            output_zero_point,
            multiplier,
            0,
            1,
            hstart,
            hend,
            wstart,
            wend,
            1,
            1,
            inputWidth,
            1);
        // 1) The following loop handles the remaining channels
        // 2) It also handles the Non-AVX2 path
        for (; c < nInputPlane; ++c) {
          int32_t acc_int32 = input_zero_point_m_size;
          int64_t tcntr = 0;
          for (int64_t ih = hstart; ih < hend; ih++) {
            for (int64_t iw = wstart; iw < wend; iw++) {
              tcntr = ih * inputWidth + iw;
              auto val = *(i_p + tcntr * nInputPlane + c);
              acc_int32 += val;
            }
          }
          double acc_fp = acc_int32 * 1.0;
          // clamp
          o_p[c] = at::quantize_val<scalar_t>(
                       1.0f / multiplier, output_zero_point, acc_fp)
                       .val_;
        } // c
      } // ow
    } // oh
  });
}

void qavg_pool3d_nhwc_kernel(
    const Tensor& qx,
    Tensor& qy,
    int64_t b,
    int64_t nInputPlane,
    int64_t inputWidth,
    int64_t inputHeight,
    int64_t inputDepth,
    int64_t outputWidth,
    int64_t outputHeight,
    int64_t outputDepth,
    int kW,
    int kH,
    int kD,
    int dW,
    int dH,
    int dD,
    int padW,
    int padH,
    int padD,
    bool count_include_pad,
    c10::optional<int64_t> divisor_override) {
  AT_DISPATCH_QINT_TYPES(qx.scalar_type(), "avg_pool3d_nhwc", [&]() {
    scalar_t* idata = static_cast<scalar_t*>(qx.data_ptr());
    scalar_t* odata = static_cast<scalar_t*>(qy.data_ptr());
    int batch_size = nInputPlane * inputWidth * inputHeight * inputDepth;
    auto* i_p = reinterpret_cast<typename scalar_t::underlying*>(
        idata + b * batch_size);

    // lift these operations outside the loop to reduce access overheads
    float input_scale = qx.q_scale();
    float output_scale = qy.q_scale();
    int input_zero_point = qx.q_zero_point();
    int output_zero_point = qy.q_zero_point();
    int64_t divisor_override_factor =
        divisor_override.has_value() ? divisor_override.value() : 0;

    for (int od = 0; od < outputDepth; od++) {
      for (int oh = 0; oh < outputHeight; oh++) {
        for (int ow = 0; ow < outputWidth; ow++) {
          auto* o_p = reinterpret_cast<typename scalar_t::underlying*>(
              odata +
              b * nInputPlane * outputWidth * outputHeight * outputDepth +
              (od * outputHeight * outputWidth + oh * outputWidth + ow) *
                  nInputPlane);
          int dstart = od * dD - padD;
          int hstart = oh * dH - padH;
          int wstart = ow * dW - padW;

          int dend = std::min(dstart + kD, (int)inputDepth + padD);
          int hend = std::min(hstart + kH, (int)inputHeight + padH);
          int wend = std::min(wstart + kW, (int)inputWidth + padW);
          int pool_size = (dend - dstart) * (hend - hstart) * (wend - wstart);

          dstart = std::max(dstart, 0);
          hstart = std::max(hstart, 0);
          wstart = std::max(wstart, 0);
          dend = std::min(dend, (int)inputDepth);
          hend = std::min(hend, (int)inputHeight);
          wend = std::min(wend, (int)inputWidth);

          int size = (dend - dstart) * (hend - hstart) * (wend - wstart);
          int divide_size = count_include_pad ? pool_size : size;
          int divide_factor =
              divisor_override_factor ? divisor_override_factor : divide_size;
          float multiplier = input_scale / output_scale / divide_factor;
          int input_zero_point_m_size = -input_zero_point * size;

          int c_start = 0;

          // For int8 quantization, we implicitly use int32 as accumulation
          // Or else, it will go to the slow path
          // TODO: support 16bit, 32bit, and etc.
          do_avg_pool_nhwc_on_AVX2<scalar_t>(
              i_p,
              o_p,
              c_start,
              input_zero_point_m_size,
              output_zero_point,
              multiplier,
              dstart,
              dend,
              hstart,
              hend,
              wstart,
              wend,
              inputDepth,
              inputHeight,
              inputWidth,
              nInputPlane);

          // 1) The following loop handles the remaining channels
          // 2) It also handles the Non-AVX2 path
          for (int c = c_start; c < nInputPlane; ++c) {
            int32_t acc_int32 = input_zero_point_m_size;
            int64_t tcntr = 0;
            for (int64_t id = dstart; id < dend; id++) {
              for (int64_t ih = hstart; ih < hend; ih++) {
                for (int64_t iw = wstart; iw < wend; iw++) {
                  tcntr = id * inputHeight * inputWidth + ih * inputWidth + iw;
                  auto val = *(i_p + tcntr * nInputPlane + c);
                  acc_int32 += val;
                }
              }
            }
            double acc_fp = acc_int32 * 1.0;
            // clamp
            o_p[c] = at::quantize_val<scalar_t>(
                         1.0f / multiplier, output_zero_point, acc_fp)
                         .val_;
          } // c
        } // ow
      } // oh
    } // od
  });
} // namespace

template <typename T>
int64_t do_quantized_bilinear_on_AVX2(
    const typename T::underlying*& pos1,
    typename T::underlying*& pos2,
    int64_t input_height,
    int64_t input_width,
    int64_t output_height,
    int64_t output_width,
    int64_t channels,
    int32_t output_zero_point,
    int32_t input_zero_point,
    float inverse_scale,
    const float h0lambda,
    const float h1lambda,
    const float w0lambda,
    const float w1lambda,
    const int64_t h1p,
    const int64_t w1p) {
  int64_t c = 0;
#if defined(__AVX2__) && !defined(_MSC_VER)
  constexpr auto vec_width = Vec256<T>::size() / 4;
  if (vec_width == 8) {
    for (; c + vec_width <= channels; c += vec_width) {
      Vec256<float> pos1_fp_v[4];
      Vec256<int32_t> pos1_int_v[4];
      pos1_int_v[0] = vec256::convert_to_int32<typename T::underlying>(pos1);
      pos1_int_v[1] = vec256::convert_to_int32<typename T::underlying>(
          pos1 + w1p * channels);
      pos1_int_v[2] = vec256::convert_to_int32<typename T::underlying>(
          pos1 + h1p * input_width * channels);
      pos1_int_v[3] = vec256::convert_to_int32<typename T::underlying>(
          pos1 + (h1p * input_width + w1p) * channels);
      for (int i = 0; i < 4; i++) {
        int32_t pos1_int[vec_width];
        float pos1_fp[vec_width];
        pos1_int_v[i].store(pos1_int);
        vec256::convert(pos1_int, pos1_fp, vec_width);
        pos1_fp_v[i] = Vec256<float>::loadu(pos1_fp, 8);
      }
      Vec256<float> h0lambda_v(h0lambda);
      Vec256<float> h1lambda_v(h1lambda);
      Vec256<float> w0lambda_v(w0lambda);
      Vec256<float> w1lambda_v(w1lambda);
      Vec256<float> input_zero_point_v(input_zero_point);
      Vec256<float> result =
          h0lambda_v * (w0lambda_v * pos1_fp_v[0] + w1lambda_v * pos1_fp_v[1]) +
          h1lambda_v * (w0lambda_v * pos1_fp_v[2] + w1lambda_v * pos1_fp_v[3]) -
          input_zero_point_v;
      float result_fp[vec_width];
      result.store(result_fp);
      at::quantize_vec<T>(
          inverse_scale,
          output_zero_point,
          result_fp,
          reinterpret_cast<T*>(pos2),
          vec_width);
      pos1 += vec_width;
      pos2 += vec_width;
    }
  }
#endif
  return c;
}

void qupsample_bilinear2d_nhwc_kernel(
    Tensor& output,
    const Tensor& input,
    int64_t input_height,
    int64_t input_width,
    int64_t output_height,
    int64_t output_width,
    int64_t nbatch,
    int64_t channels,
    bool align_corners,
    c10::optional<double> scales_h,
    c10::optional<double> scales_w) {
  AT_DISPATCH_QINT_TYPES(
      input.scalar_type(), "upsample_bilinear2d_nhwc", [&]() {
        auto* idata = static_cast<scalar_t*>(input.data_ptr());
        auto* odata = static_cast<scalar_t*>(output.data_ptr());
        float inverse_scale = output.q_scale() / input.q_scale();
        const auto rheight = area_pixel_compute_scale<float>(
            input_height, output_height, align_corners, scales_h);
        const auto rwidth = area_pixel_compute_scale<float>(
            input_width, output_width, align_corners, scales_w);

        for (int64_t b = 0; b < nbatch; ++b) {
          auto* i_p = reinterpret_cast<typename scalar_t::underlying*>(
              idata + b * input_height * input_width * channels);
          auto* o_p = reinterpret_cast<typename scalar_t::underlying*>(
              odata + b * output_height * output_width * channels);

          for (int64_t h2 = 0; h2 < output_height; ++h2) {
            const auto h1r = area_pixel_compute_source_index<float>(
                rheight, h2, align_corners, /*cubic=*/false);

            const int64_t h1 = h1r;
            const int64_t h1p = (h1 < input_height - 1) ? 1 : 0;
            const float h1lambda = h1r - h1;
            const float h0lambda = static_cast<float>(1.) - h1lambda;

            for (int64_t w2 = 0; w2 < output_width; ++w2) {
              const auto w1r = area_pixel_compute_source_index<float>(
                  rwidth, w2, align_corners, /*cubic=*/false);
              const int64_t w1 = w1r;
              const int64_t w1p = (w1 < input_width - 1) ? 1 : 0;

              const float w1lambda = w1r - w1;
              const float w0lambda = static_cast<float>(1.) - w1lambda;

              int64_t c = 0;
              // We use float32 to do the computation
              const typename scalar_t::underlying* pos1 =
                  i_p + (h1 * input_width + w1) * channels;
              typename scalar_t::underlying* pos2 =
                  o_p + (h2 * output_width + w2) * channels;
              // We have to isolate this function out because the VS does not
              // expand the macro correctly.
              c = do_quantized_bilinear_on_AVX2<scalar_t>(
                  pos1,
                  pos2,
                  input_height,
                  input_width,
                  output_height,
                  output_width,
                  channels,
                  output.q_zero_point(),
                  input.q_zero_point(),
                  inverse_scale,
                  h0lambda,
                  h1lambda,
                  w0lambda,
                  w1lambda,
                  h1p,
                  w1p);
              // 1) The following loop handles the remaining channels
              // 2) It also handles the Non-AVX2 path
              for (; c < channels; ++c) {
                float result = h0lambda *
                        (w0lambda * pos1[0] + w1lambda * pos1[w1p * channels]) +
                    h1lambda *
                        (w0lambda * pos1[h1p * input_width * channels] +
                         w1lambda * pos1[(h1p * input_width + w1p) * channels]);
                pos2[0] = at::quantize_val<scalar_t>(
                              inverse_scale,
                              output.q_zero_point(),
                              result - input.q_zero_point())
                              .val_;
                pos1 += 1;
                pos2 += 1;
              } // c
            } // w2
          } // h2
        } // b
      });
}

void qtopk_kernel(Tensor& values,
    Tensor& indices,
    const Tensor& self,
    int64_t k,
    int64_t dim,
    bool largest,
    bool sorted) {
  AT_DISPATCH_QINT_TYPES(self.scalar_type(), "qtopk_cpu", [&] {
    dim_apply(
        {self, values, indices},
        dim,
        [&](int64_t i, TensorList tl) {
          auto tmp_values = tl[0].accessor<scalar_t, 1>();
          auto mode_values = tl[1].accessor<scalar_t, 1>();
          auto mode_indices = tl[2].accessor<int64_t, 1>();

          auto n = tmp_values.size(0);
          auto use_partial_sort = k * 64 <= n;

          using elem_t = std::pair<typename scalar_t::underlying, int64_t>;
          std::vector<elem_t> queue(n);
          for (int64_t j = 0; j < n; j++) {
            queue[j].first = tmp_values[j].val_;
            queue[j].second = j;
          }

          // we want NaN to be sorted as top for numpy compatibility
          if (use_partial_sort) {
            if (largest) {
              std::partial_sort(queue.begin(), queue.begin() + k, queue.end(),
                [](const elem_t& x, const elem_t& y) -> bool {
                  return x.first > y.first;
                });
            } else {
              std::partial_sort(queue.begin(), queue.begin() + k, queue.end(),
                [](const elem_t& x, const elem_t& y) -> bool {
                  return x.first < y.first;
                });
            }
          } else {
            if (largest) {
              std::nth_element(queue.begin(), queue.begin() + k - 1, queue.end(),
                [](const elem_t& x, const elem_t& y) -> bool {
                  return x.first > y.first;
                });
              if (sorted) {
                std::sort(queue.begin(), queue.begin() + k - 1,
                  [](const elem_t& x, const elem_t& y) -> bool {
                    return x.first > y.first;
                  });
              }
            } else {
              std::nth_element(queue.begin(), queue.begin() + k -1, queue.end(),
                [](const elem_t& x, const elem_t& y) -> bool {
                  return x.first < y.first;
                });
              if (sorted) {
                std::sort(queue.begin(), queue.begin() + k -1,
                  [](const elem_t& x, const elem_t& y) -> bool {
                    return x.first < y.first;
                  });
              }
            }
          }

          for (int64_t j = 0; j < k; j++) {
            mode_values[j] = scalar_t(queue[j].first);
            mode_indices[j] = queue[j].second;
          }
        });
  });
}

template <typename T>
inline void do_bn_compute(
    typename T::underlying* X_ptr,
    typename T::underlying* Y_ptr,
    Vec256<float> & fake_scale,
    Vec256<float> & in_zp_vec,
    Vec256<float> & scale_neg_zp_premul,
    int64_t out_zero_point,
    Vec256<T> & out_zero_point_v,
    float*  alpha,
    float* beta,
    int64_t vec_num,
    bool ReluFused,
    int64_t kVLen
) {
  using Vec = Vec256<T>;
  auto vals_q = Vec::loadu(X_ptr);
  // Fake scale of 1.0 here, should not affect performance (FMA in place of sub)
  auto vals_dq = vals_q.dequantize(fake_scale, in_zp_vec, scale_neg_zp_premul);
  for (size_t idx = 0; idx < vec_num; ++idx) {
    auto alpha_v = Vec256<float>::loadu(alpha + idx * kVLen);
    auto beta_v = Vec256<float>::loadu(beta + idx * kVLen);
    vals_dq[idx] = vec256::fmadd(alpha_v, vals_dq[idx], beta_v);
  }
  auto outputs_q = Vec::quantize(vals_dq, /*output_scale=*/1.0f, out_zero_point, /*inv_output_scale=*/1.0f);
  // Fake scale again
  if (ReluFused) {
    outputs_q = outputs_q.maximum(out_zero_point_v);
  }
  outputs_q.store(Y_ptr, vec_num * kVLen);
}

template <bool ReluFused>
void q_batch_norm_kernel(
    int64_t N,
    int64_t C,
    int64_t HxW,
    int64_t in_zero_point,
    int64_t out_zero_point,
    const Tensor& input,
    const Tensor& a,
    const Tensor& b,
    Tensor& output) {

  AT_DISPATCH_QINT_TYPES(input.scalar_type(), "qbatch_norm", [&]() {
    float* alpha = a.data_ptr<float>();
    float* beta = b.data_ptr<float>();
    auto minimum = std::numeric_limits<scalar_t::underlying>::lowest();
    auto maximum = std::numeric_limits<scalar_t::underlying>::max();
    scalar_t::underlying* X =
        reinterpret_cast<scalar_t::underlying*>(input.data_ptr());
    scalar_t::underlying* Y = reinterpret_cast<scalar_t::underlying*>(output.data_ptr());

    constexpr int kVLen = 8;
    const int64_t outer_size = N * HxW;
    using Vec = Vec256<scalar_t>;
    // Hoisted variables
    auto in_zp_vec = Vec256<float>(static_cast<float>(in_zero_point));
    auto fake_scale = Vec256<float>(1.0f);
    auto scale_neg_zp_premul = fake_scale * in_zp_vec.neg();
    auto out_zero_point_v = Vec(scalar_t(out_zero_point));
    size_t lanes = Vec::float_num_vecs() * kVLen;
    for (int64_t i = 0; i < outer_size; ++i) {
      auto* X_ptr = reinterpret_cast<typename scalar_t::underlying*>(X + i * C);
      auto* Y_ptr = reinterpret_cast<typename scalar_t::underlying*>(Y + i * C);
      int64_t ch = 0;

      for(; ch + lanes <= C; ch += lanes ) {
        do_bn_compute<scalar_t>(
          X_ptr + ch,
          Y_ptr + ch,
          fake_scale,
          in_zp_vec,
          scale_neg_zp_premul,
          out_zero_point,
          out_zero_point_v,
          alpha + ch,
          beta + ch,
          Vec::float_num_vecs(),
          ReluFused,
          kVLen
        );
      }

      // for channel between 8 and 32, still use 32 width for performance
      // Benchmark shows it is faster than doing 8 channels each time
      int64_t elem_size = C - ch;
      if ((lanes == 32) && elem_size >= kVLen) {
        int64_t vec_num = elem_size / kVLen;
        std::vector<typename scalar_t::underlying> buf_in(lanes);
        memcpy(buf_in.data(), X_ptr + ch, vec_num * kVLen); // 3 cycles
        do_bn_compute<scalar_t>(
          buf_in.data(),
          Y_ptr + ch,
          fake_scale,
          in_zp_vec,
          scale_neg_zp_premul,
          out_zero_point,
          out_zero_point_v,
          alpha + ch,
          beta + ch,
          vec_num,
          ReluFused,
          kVLen
        );
        ch += vec_num * kVLen;
      }
      // for channels less than 8
      for (; ch < C; ++ch) {
        long quantized_down = out_zero_point +
            lrintf(alpha[ch] * (X_ptr[ch] - in_zero_point) +
                        beta[ch]);
        if (ReluFused) { // static if
          quantized_down = std::max<long>(quantized_down, out_zero_point);
        }
        Y_ptr[ch] = std::min<long>(
            std::max<long>(quantized_down, minimum), maximum);
      }
    }
});

}

void fake_quantize_tensor_kernel(
    Tensor& output,
    const Tensor& input,
    float sc,
    int64_t z_point,
    int64_t quant_min,
    int64_t quant_max) {
  float inv_scale = 1.0f / sc;
  auto iter = TensorIterator::unary_op(output, input);
  cpu_kernel(iter, [&](float self) -> float {
    return (std::fmin(
                std::fmax(
                    static_cast<int64_t>(
                        std::nearbyint(self * inv_scale + z_point)),
                    quant_min),
                quant_max) -
            z_point) *
        sc;
  });
}

void fake_quantize_grad_tensor_kernel(
    Tensor& input_grad,
    const Tensor& input,
    const Tensor& output_grad,
    float sc,
    int64_t z_point,
    int64_t quant_min,
    int64_t quant_max) {
  float inv_scale = 1.0f / sc;
  auto iter = TensorIterator::binary_op(input_grad, input, output_grad);
  cpu_kernel(iter, [&](float x, float dy) -> float {
    int64_t xq = static_cast<int64_t>(std::nearbyint(x * inv_scale + z_point));
    return dy * (xq >= quant_min && xq <= quant_max);
  });
}

void fake_quant_per_channel_cpu(TensorIterator &iter, int64_t quant_min, int64_t quant_max) {
  cpu_kernel(iter,
    [=](float self, float scale, int64_t zero_point) -> float {
      float inv_scale = 1.0f / scale;
      return (std::fmin(
                std::fmax(
                    static_cast<int64_t>(
                        std::nearbyint(self * inv_scale + zero_point)),
                    quant_min),
                quant_max) -
            zero_point) *
        scale;
    });
}

void fake_quant_grad_per_channel_cpu(TensorIterator &iter, int64_t quant_min, int64_t quant_max) {
  cpu_kernel(iter,
    [=](float x, float dy, float scale, int64_t zero_point) -> float {
      float inv_scale = 1.0f / scale;
      int64_t xq = static_cast<int64_t>(std::nearbyint(x * inv_scale + zero_point));
      return dy * (xq >= quant_min && xq <= quant_max);
    });
}

template <typename T>
void quantized_layer_norm_kernel_impl(
    const Tensor& X,
    const Tensor& gamma,
    const Tensor& beta,
    int64_t M,
    int64_t N,
    float eps,
    Tensor* Y) {

}

void quantized_layer_norm_kernel(
    const Tensor& X,
    const Tensor& gamma,
    const Tensor& beta,
    int64_t M,
    int64_t N,
    double eps,
    Tensor* Y) {
  AT_DISPATCH_QINT_TYPES(X.scalar_type(), "quantized_layer_norm_kernel_impl_cpu", [&]() {
    using qVec = vec256::Vec256<scalar_t>;
    using fVec = vec256::Vec256<float>;

    TORCH_INTERNAL_ASSERT(X.numel() == M * N, "Unexpected num elements in X");
    TORCH_INTERNAL_ASSERT(!gamma.defined() || gamma.numel() == N,
        "Unexpected size of gamma");
    TORCH_INTERNAL_ASSERT(!beta.defined() || beta.numel() == N,
        "Unexpected size of beta");
    scalar_t* X_data = X.data_ptr<scalar_t>();
    const float* gamma_data = gamma.defined() ? gamma.data_ptr<float>() : nullptr;
    const float* beta_data = beta.defined() ? beta.data_ptr<float>() : nullptr;
    scalar_t* Y_data = Y->data_ptr<scalar_t>();
    const bool gamma_null = gamma_data == nullptr;
    const bool beta_null = beta_data == nullptr;
    int64_t x_zp = X.q_zero_point();
    float x_scale = X.q_scale();
    fVec x_zp_vec((float)x_zp);
    fVec one_vec(1.0f);
    fVec zero_vec(0.0f);
    float x_fake_scale = 1.0f;
    fVec x_fake_scale_vec(x_fake_scale);
    fVec x_fake_scale_zp_neg_premul_vec = x_fake_scale_vec * x_zp_vec.neg();
    int64_t y_zp = Y->q_zero_point();
    float y_scale = Y->q_scale();
    float y_inv_scale = 1.0f / y_scale;

    constexpr int kFloatVLen = 8;
    int64_t kIntVLen = kFloatVLen * qVec::float_num_vecs();
    int64_t kNumIntVecInLayer = N / kIntVLen;
    int64_t kNonVecRemInLayer = N % kIntVLen;

    at::parallel_for(0, M, 1, [&](int64_t start, int64_t end) {
      for (int64_t i = start; i < end; ++i) {

        scalar_t* X_ptr = X_data + i * N;
        scalar_t* Y_ptr = Y_data + i * N;

        // First pass: calculate mean and variance.

        scalar_t::underlying* X_ptr_underlying = reinterpret_cast<scalar_t::underlying*>(X_ptr);
        auto l_sum_shifted = hsum(X_ptr_underlying, N);
        auto l_sum_sq_shifted = hsum_sq(X_ptr_underlying, N);
        float l_mean_shifted_div_scale_x = static_cast<float>(l_sum_shifted) / N;
        // mean(dqX) / scale_x
        float layer_mean_div_scale_x = l_mean_shifted_div_scale_x - x_zp;
        // var(dqX) / scale_x^2
        float layer_var_div_scale_x_sq =
          std::max(static_cast<float>(l_sum_sq_shifted) / N -
              l_mean_shifted_div_scale_x * l_mean_shifted_div_scale_x, 0.0f);
        // scale_x / sqrt(var(dqX) + eps)
        float scale_x_div_layer_std = x_scale /
          std::sqrt(layer_var_div_scale_x_sq * x_scale * x_scale + eps);
        fVec layer_mean_div_scale_xVec(layer_mean_div_scale_x);
        fVec scale_x_div_layer_stdVec(scale_x_div_layer_std);

        // Second pass: normalize

        // TODO replace with TensorIterator implementation once #33166 is fixed.
        for (int64_t vecIdx = 0; vecIdx < kNumIntVecInLayer; vecIdx++) {
          int64_t vecStartIdx = vecIdx * kIntVLen;
          auto qXVec = qVec::loadu(X_ptr + vecStartIdx);
          auto dqXVec = qXVec.dequantize(x_fake_scale_vec, x_zp_vec,
              x_fake_scale_zp_neg_premul_vec);
          for (int dqXVecIdx = 0; dqXVecIdx < dqXVec.size(); dqXVecIdx++) {
            int64_t vecVecStartIdx = vecStartIdx + dqXVecIdx * kFloatVLen;
            auto gammaVec = gamma_null
              ? one_vec
              : fVec::loadu(gamma_data + vecVecStartIdx);
            auto betaVec = beta_null
              ? zero_vec
              : fVec::loadu(beta_data + vecVecStartIdx);
            dqXVec[dqXVecIdx] =
              (dqXVec[dqXVecIdx] - layer_mean_div_scale_xVec) *
                scale_x_div_layer_stdVec * gammaVec + betaVec;
            qVec::quantize(dqXVec, y_scale, y_zp, y_inv_scale)
              .store(Y_ptr + vecStartIdx);
          }
        }
        for (int64_t remIdx = N - kNonVecRemInLayer; remIdx < N; remIdx++) {
          const float gamma_v = gamma_null ? 1.0f : gamma_data[remIdx];
          const float beta_v = beta_null ? 0.0f : beta_data[remIdx];
          auto qXVal = X_ptr[remIdx];
          float dqXVal = at::dequantize_val(x_fake_scale, x_zp, qXVal);
          float dqY =
            ((dqXVal - layer_mean_div_scale_x) * scale_x_div_layer_std) * gamma_v + beta_v;
          Y_ptr[remIdx] = at::quantize_val<scalar_t>(y_scale, y_zp, dqY);
        }
      }
    }); // parallel_for

  });
}

} // namespace

REGISTER_DISPATCH(qrelu_stub, &qrelu_kernel);
REGISTER_DISPATCH(qrelu6_stub, &qrelu6_kernel);
REGISTER_DISPATCH(qrelu_leaky_stub, &leaky_qrelu_out_kernel);
REGISTER_DISPATCH(qsigmoid_stub, &qsigmoid_kernel);
REGISTER_DISPATCH(qhardsigmoid_stub, &qhardsigmoid_kernel);
REGISTER_DISPATCH(qclamp_stub, &qclamp_kernel);
REGISTER_DISPATCH(qtanh_stub, &qtanh_kernel);
REGISTER_DISPATCH(qhardswish_stub, &qhardswish_kernel);
REGISTER_DISPATCH(qelu_stub, &qelu_kernel);
REGISTER_DISPATCH(qadd_relu_stub, &qadd_kernel<true>);
REGISTER_DISPATCH(qadd_stub, &qadd_kernel<false>);
REGISTER_DISPATCH(qadd_scalar_relu_stub, &qadd_scalar_kernel<true>);
REGISTER_DISPATCH(qadd_scalar_stub, &qadd_scalar_kernel<false>);
REGISTER_DISPATCH(qmul_relu_stub, &qmul_kernel<true>);
REGISTER_DISPATCH(qmul_stub, &qmul_kernel<false>);
REGISTER_DISPATCH(qmaxpool_2d_nhwc_stub, &qmaxpool_2d_nhwc_kernel);
REGISTER_DISPATCH(
    qadaptive_avg_pool2d_nhwc_stub,
    &qadaptive_avg_pool2d_nhwc_kernel);
REGISTER_DISPATCH(qavg_pool2d_nhwc_stub, &qavg_pool2d_nhwc_kernel);
REGISTER_DISPATCH(qavg_pool3d_nhwc_stub, &qavg_pool3d_nhwc_kernel);
REGISTER_DISPATCH(
    qupsample_bilinear2d_nhwc_stub,
    &qupsample_bilinear2d_nhwc_kernel);
REGISTER_DISPATCH(qcat_nhwc_stub, &qcat_nhwc_kernel<false>);
REGISTER_DISPATCH(qcat_relu_nhwc_stub, &qcat_nhwc_kernel<true>);
REGISTER_DISPATCH(qtopk_stub, &qtopk_kernel);
REGISTER_DISPATCH(qbatch_norm_stub, &q_batch_norm_kernel<false>);
REGISTER_DISPATCH(qbatch_norm_relu_stub, &q_batch_norm_kernel<true>);
REGISTER_DISPATCH(fake_quant_tensor_stub, &fake_quantize_tensor_kernel);
REGISTER_DISPATCH(fake_quant_grad_tensor_stub, &fake_quantize_grad_tensor_kernel);
REGISTER_DISPATCH(fake_quant_per_channel_stub, &fake_quant_per_channel_cpu);
REGISTER_DISPATCH(fake_quant_grad_per_channel_stub, &fake_quant_grad_per_channel_cpu);
REGISTER_DISPATCH(quantized_layer_norm_stub, &quantized_layer_norm_kernel);

} // namespace native
} // namespace at
