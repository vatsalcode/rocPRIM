// Copyright (c) 2017 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#ifndef ROCPRIM_DEVICE_DEVICE_HISTOGRAM_HC_HPP_
#define ROCPRIM_DEVICE_DEVICE_HISTOGRAM_HC_HPP_

#include <cmath>
#include <type_traits>
#include <iterator>

#include "../config.hpp"
#include "../functional.hpp"
#include "../detail/various.hpp"

#include "detail/device_histogram.hpp"

BEGIN_ROCPRIM_NAMESPACE

/// \addtogroup devicemodule_hc
/// @{

namespace detail
{

#define ROCPRIM_DETAIL_HC_SYNC(name, size, start) \
    { \
        if(debug_synchronous) \
        { \
            std::cout << name << "(" << size << ")"; \
            acc_view.wait(); \
            auto end = std::chrono::high_resolution_clock::now(); \
            auto d = std::chrono::duration_cast<std::chrono::duration<double>>(end - start); \
            std::cout << " " << d.count() * 1000 << " ms" << '\n'; \
        } \
    }

template<
    unsigned int Channels,
    unsigned int ActiveChannels,
    class SampleIterator,
    class Counter,
    class SampleToBinOp
>
inline
void histogram_impl(void * temporary_storage,
                    size_t& storage_size,
                    SampleIterator samples,
                    unsigned int columns,
                    unsigned int rows,
                    size_t row_stride_bytes,
                    Counter * histogram[ActiveChannels],
                    unsigned int levels[ActiveChannels],
                    SampleToBinOp sample_to_bin_op[ActiveChannels],
                    hc::accelerator_view& acc_view,
                    bool debug_synchronous)
{
    using sample_type = typename std::iterator_traits<SampleIterator>::value_type;

    constexpr unsigned int block_size = 256;
    constexpr unsigned int items_per_thread = 8;
    constexpr unsigned int max_grid_size = 1024;
    constexpr unsigned int shared_impl_max_bins = 1024;

    constexpr unsigned int items_per_block = block_size * items_per_thread;

    for(unsigned int channel = 0; channel < ActiveChannels; channel++)
    {
        if(levels[channel] < 2)
        {
            // Histogram must have at least 1 bin
            throw hc::runtime_exception("`levels` must be at least 2", 0);
        }
    }

    if(row_stride_bytes % sizeof(sample_type) != 0)
    {
        // Row stride must be a whole multiple of the sample data type size
        throw hc::runtime_exception("Row stride must be a whole multiple of the sample data type size", 0);
    }

    const unsigned int blocks_x = ::rocprim::detail::ceiling_div(columns, items_per_block);
    const unsigned int row_stride = row_stride_bytes / sizeof(sample_type);

    if(temporary_storage == nullptr)
    {
        // Make sure user won't try to allocate 0 bytes memory, otherwise
        // user may again pass nullptr as temporary_storage
        storage_size = 4;
        return;
    }

    if(debug_synchronous)
    {
        std::cout << "columns " << columns << '\n';
        std::cout << "rows " << rows << '\n';
        std::cout << "blocks_x " << blocks_x << '\n';
        acc_view.wait();
    }

    unsigned int bins[ActiveChannels];
    unsigned int bins_bits[ActiveChannels];
    unsigned int total_bins = 0;
    unsigned int max_bins = 0;
    for(unsigned int channel = 0; channel < ActiveChannels; channel++)
    {
        bins[channel] = levels[channel] - 1;
        bins_bits[channel] = static_cast<unsigned int>(std::log2(detail::next_power_of_two(bins[channel])));
        total_bins += bins[channel];
        max_bins = std::max(max_bins, bins[channel]);
    }

    fixed_array<Counter *, ActiveChannels> histogram_fixed(histogram);
    fixed_array<unsigned int, ActiveChannels> bins_fixed(bins);
    fixed_array<unsigned int, ActiveChannels> bins_bits_fixed(bins_bits);

    // Workaround: HCC cannot pass structs with array fields of composite types
    // even with custom serializer and deserializer (see fixed_array).
    // It seems that they work only with primitive types).
    // Hence a new fixed_array is recreated inside kernels using individual values that can be passed correctly.
    auto sample_to_bin_op0 = sample_to_bin_op[std::min(0u, ActiveChannels - 1)];
    auto sample_to_bin_op1 = sample_to_bin_op[std::min(1u, ActiveChannels - 1)];
    auto sample_to_bin_op2 = sample_to_bin_op[std::min(2u, ActiveChannels - 1)];
    auto sample_to_bin_op3 = sample_to_bin_op[std::min(3u, ActiveChannels - 1)];

    std::chrono::high_resolution_clock::time_point start;

    if(debug_synchronous) start = std::chrono::high_resolution_clock::now();
    hc::parallel_for_each(
        acc_view,
        hc::tiled_extent<1>(::rocprim::detail::ceiling_div(max_bins, block_size) * block_size, block_size),
        [=](hc::tiled_index<1>) [[hc]]
        {
            init_histogram<block_size, ActiveChannels>(histogram_fixed, bins_fixed);
        }
    );
    ROCPRIM_DETAIL_HC_SYNC("init_histogram", max_bins, start);

    if(total_bins <= shared_impl_max_bins)
    {
        const unsigned int grid_size_x = std::min(max_grid_size, blocks_x);
        const unsigned int grid_size_y = std::min(rows, max_grid_size / grid_size_x);
        const size_t block_histogram_bytes = total_bins * sizeof(unsigned int);
        if(debug_synchronous) start = std::chrono::high_resolution_clock::now();
        hc::parallel_for_each(
            acc_view,
            hc::tiled_extent<2>(grid_size_y, grid_size_x * block_size, 1, block_size, block_histogram_bytes),
            [=](hc::tiled_index<2>) [[hc]]
            {
                fixed_array<SampleToBinOp, ActiveChannels> sample_to_bin_op_fixed(
                    sample_to_bin_op0, sample_to_bin_op1, sample_to_bin_op2, sample_to_bin_op3
                );

                unsigned int * block_histogram = static_cast<unsigned int *>(
                    hc::get_dynamic_group_segment_base_pointer()
                );

                histogram_shared<block_size, items_per_thread, Channels, ActiveChannels>(
                    samples, columns, rows, row_stride,
                    histogram_fixed,
                    sample_to_bin_op_fixed,
                    bins_fixed,
                    block_histogram
                );
            }
        );
        ROCPRIM_DETAIL_HC_SYNC("histogram_shared", grid_size_x * grid_size_y * block_size, start);
    }
    else
    {
        if(debug_synchronous) start = std::chrono::high_resolution_clock::now();
        hc::parallel_for_each(
            acc_view,
            hc::tiled_extent<2>(rows, blocks_x * block_size, 1, block_size),
            [=](hc::tiled_index<2>) [[hc]]
            {
                fixed_array<SampleToBinOp, ActiveChannels> sample_to_bin_op_fixed(
                    sample_to_bin_op0, sample_to_bin_op1, sample_to_bin_op2, sample_to_bin_op3
                );

                histogram_global<block_size, items_per_thread, Channels, ActiveChannels>(
                    samples, columns, row_stride,
                    histogram_fixed,
                    sample_to_bin_op_fixed,
                    bins_bits_fixed
                );
            }
        );
        ROCPRIM_DETAIL_HC_SYNC("histogram_global", blocks_x * block_size * rows, start);
    }
}

template<
    unsigned int Channels,
    unsigned int ActiveChannels,
    class SampleIterator,
    class Counter,
    class Level
>
inline
void histogram_even_impl(void * temporary_storage,
                         size_t& storage_size,
                         SampleIterator samples,
                         unsigned int columns,
                         unsigned int rows,
                         size_t row_stride_bytes,
                         Counter * histogram[ActiveChannels],
                         unsigned int levels[ActiveChannels],
                         Level lower_level[ActiveChannels],
                         Level upper_level[ActiveChannels],
                         hc::accelerator_view& acc_view,
                         bool debug_synchronous)
{
    sample_to_bin_even<Level> sample_to_bin_op[ActiveChannels];
    for(unsigned int channel = 0; channel < ActiveChannels; channel++)
    {
        sample_to_bin_op[channel] = sample_to_bin_even<Level>(
            levels[channel] - 1,
            lower_level[channel], upper_level[channel]
        );
    }

    histogram_impl<Channels, ActiveChannels>(
        temporary_storage, storage_size,
        samples, columns, rows, row_stride_bytes,
        histogram,
        levels, sample_to_bin_op,
        acc_view, debug_synchronous
    );
}

template<
    unsigned int Channels,
    unsigned int ActiveChannels,
    class SampleIterator,
    class Counter,
    class Level
>
inline
void histogram_range_impl(void * temporary_storage,
                          size_t& storage_size,
                          SampleIterator samples,
                          unsigned int columns,
                          unsigned int rows,
                          size_t row_stride_bytes,
                          Counter * histogram[ActiveChannels],
                          unsigned int levels[ActiveChannels],
                          Level * level_values[ActiveChannels],
                          hc::accelerator_view& acc_view,
                          bool debug_synchronous)
{
    sample_to_bin_range<Level> sample_to_bin_op[ActiveChannels];
    for(unsigned int channel = 0; channel < ActiveChannels; channel++)
    {
        sample_to_bin_op[channel] = sample_to_bin_range<Level>(
            levels[channel] - 1,
            level_values[channel]
        );
    }

    histogram_impl<Channels, ActiveChannels>(
        temporary_storage, storage_size,
        samples, columns, rows, row_stride_bytes,
        histogram,
        levels, sample_to_bin_op,
        acc_view, debug_synchronous
    );
}

#undef ROCPRIM_DETAIL_HC_SYNC

} // end of detail namespace

/// \brief Computes a histogram from a sequence of samples using equal-width bins.
///
/// \par
/// * The number of histogram bins is (\p levels - 1).
/// * Bins are evenly-segmented and include the same width of sample values:
/// (\p upper_level - \p lower_level) / (\p levels - 1).
/// * Returns the required size of \p temporary_storage in \p storage_size
/// if \p temporary_storage in a null pointer.
///
/// \tparam SampleIterator - random-access iterator type of the input range. Must meet the
/// requirements of a C++ InputIterator concept. It can be a simple pointer type.
/// \tparam Counter - integer type for histogram bin counters.
/// \tparam Level - type of histogram boundaries (levels)
///
/// \param [in] temporary_storage - pointer to a device-accessible temporary storage. When
/// a null pointer is passed, the required allocation size (in bytes) is written to
/// \p storage_size and function returns without performing the reduction operation.
/// \param [in,out] storage_size - reference to a size (in bytes) of \p temporary_storage.
/// \param [in] samples - iterator to the first element in the range of input samples.
/// \param [in] size - number of elements in the samples range.
/// \param [out] histogram - pointer to the first element in the histogram range.
/// \param [in] levels - number of boundaries (levels) for histogram bins.
/// \param [in] lower_level - lower sample value bound (inclusive) for the first histogram bin.
/// \param [in] upper_level - upper sample value bound (exclusive) for the last histogram bin.
/// \param [in] acc_view - [optional] \p hc::accelerator_view object. The default value
/// is \p hc::accelerator().get_default_view() (default view of the default accelerator).
/// \param [in] debug_synchronous - [optional] If true, synchronization after every kernel
/// launch is forced in order to check for errors. Default value is \p false.
///
/// \par Example
/// \parblock
/// In this example a device-level histogram of 5 bins is computed on an array of float samples.
///
/// \code{.cpp}
/// #include <rocprim/rocprim.hpp>
///
/// hc::accelerator_view acc_view = ...;
///
/// // Prepare input and output (declare pointers, allocate device memory etc.)
/// unsigned int size;        // e.g., 8
/// hc::array<float> samples; // e.g., [-10.0, 0.3, 9.5, 8.1, 1.5, 1.9, 100.0, 5.1]
/// hc::array<int> histogram; // empty array of at least 5 elements
/// unsigned int levels;      // e.g., 6 (for 5 bins)
/// float lower_level;        // e.g., 0.0
/// float upper_level;        // e.g., 10.0
///
/// size_t temporary_storage_size_bytes;
/// // Get required size of the temporary storage
/// rocprim::histogram_even(
///     nullptr, temporary_storage_size_bytes,
///     samples.accelerator_pointer(), size,
///     histogram.accelerator_pointer(), levels, lower_level, upper_level,
///     acc_view
/// );
///
/// // allocate temporary storage
/// hc::array<char> temporary_storage(temporary_storage_size_bytes, acc_view);
///
/// // compute histogram
/// rocprim::histogram_even(
///     temporary_storage.accelerator_pointer(), temporary_storage_size_bytes,
///     samples.accelerator_pointer(), size,
///     histogram.accelerator_pointer(), levels, lower_level, upper_level,
///     acc_view
/// );
/// // histogram: [3, 0, 1, 0, 2]
/// \endcode
/// \endparblock
template<
    class SampleIterator,
    class Counter,
    class Level
>
inline
void histogram_even(void * temporary_storage,
                    size_t& storage_size,
                    SampleIterator samples,
                    unsigned int size,
                    Counter * histogram,
                    unsigned int levels,
                    Level lower_level,
                    Level upper_level,
                    hc::accelerator_view acc_view = hc::accelerator().get_default_view(),
                    bool debug_synchronous = false)
{
    Counter * histogram_single[1] = { histogram };
    unsigned int levels_single[1] = { levels };
    Level lower_level_single[1] = { lower_level };
    Level upper_level_single[1] = { upper_level };

    detail::histogram_even_impl<1, 1>(
        temporary_storage, storage_size,
        samples, size, 1, 0,
        histogram_single,
        levels_single, lower_level_single, upper_level_single,
        acc_view, debug_synchronous
    );
}

/// \brief Computes a histogram from a two-dimensional region of samples using equal-width bins.
///
/// \par
/// * The two-dimensional region of interest within \p samples can be specified using the \p columns,
/// \p rows and \p row_stride_bytes parameters.
/// * The row stride must be a whole multiple of the sample data type size,
/// i.e., <tt>(row_stride_bytes % sizeof(std::iterator_traits<SampleIterator>::value_type)) == 0</tt>.
/// * The number of histogram bins is (\p levels - 1).
/// * Bins are evenly-segmented and include the same width of sample values:
/// (\p upper_level - \p lower_level) / (\p levels - 1).
/// * Returns the required size of \p temporary_storage in \p storage_size
/// if \p temporary_storage in a null pointer.
///
/// \tparam SampleIterator - random-access iterator type of the input range. Must meet the
/// requirements of a C++ InputIterator concept. It can be a simple pointer type.
/// \tparam Counter - integer type for histogram bin counters.
/// \tparam Level - type of histogram boundaries (levels)
///
/// \param [in] temporary_storage - pointer to a device-accessible temporary storage. When
/// a null pointer is passed, the required allocation size (in bytes) is written to
/// \p storage_size and function returns without performing the reduction operation.
/// \param [in,out] storage_size - reference to a size (in bytes) of \p temporary_storage.
/// \param [in] samples - iterator to the first element in the range of input samples.
/// \param [in] columns - number of elements in each row of the region.
/// \param [in] rows - number of rows of the region.
/// \param [in] row_stride_bytes - number of bytes between starts of consecutive rows of the region.
/// \param [out] histogram - pointer to the first element in the histogram range.
/// \param [in] levels - number of boundaries (levels) for histogram bins.
/// \param [in] lower_level - lower sample value bound (inclusive) for the first histogram bin.
/// \param [in] upper_level - upper sample value bound (exclusive) for the last histogram bin.
/// \param [in] acc_view - [optional] \p hc::accelerator_view object. The default value
/// is \p hc::accelerator().get_default_view() (default view of the default accelerator).
/// \param [in] debug_synchronous - [optional] If true, synchronization after every kernel
/// launch is forced in order to check for errors. Default value is \p false.
///
/// \par Example
/// \parblock
/// In this example a device-level histogram of 5 bins is computed on an array of float samples.
///
/// \code{.cpp}
/// #include <rocprim/rocprim.hpp>
///
/// hc::accelerator_view acc_view = ...;
///
/// // Prepare input and output (declare pointers, allocate device memory etc.)
/// unsigned int columns;     // e.g., 4
/// unsigned int rows;        // e.g., 2
/// size_t row_stride_bytes;  // e.g., 6 * sizeof(float)
/// hc::array<float> samples; // e.g., [-10.0, 0.3, 9.5, 8.1, -, -, 1.5, 1.9, 100.0, 5.1, -, -]
/// hc::array<int> histogram; // empty array of at least 5 elements
/// unsigned int levels;      // e.g., 6 (for 5 bins)
/// float lower_level;        // e.g., 0.0
/// float upper_level;        // e.g., 10.0
///
/// size_t temporary_storage_size_bytes;
/// // Get required size of the temporary storage
/// rocprim::histogram_even(
///     nullptr, temporary_storage_size_bytes,
///     samples.accelerator_pointer(), columns, rows, row_stride_bytes,
///     histogram.accelerator_pointer(), levels, lower_level, upper_level,
///     acc_view
/// );
///
/// // allocate temporary storage
/// hc::array<char> temporary_storage(temporary_storage_size_bytes, acc_view);
///
/// // compute histogram
/// rocprim::histogram_even(
///     temporary_storage.accelerator_pointer(), temporary_storage_size_bytes,
///     samples.accelerator_pointer(), columns, rows, row_stride_bytes,
///     histogram.accelerator_pointer(), levels, lower_level, upper_level,
///     acc_view
/// );
/// // histogram: [3, 0, 1, 0, 2]
/// \endcode
/// \endparblock
template<
    class SampleIterator,
    class Counter,
    class Level
>
inline
void histogram_even(void * temporary_storage,
                    size_t& storage_size,
                    SampleIterator samples,
                    unsigned int columns,
                    unsigned int rows,
                    size_t row_stride_bytes,
                    Counter * histogram,
                    unsigned int levels,
                    Level lower_level,
                    Level upper_level,
                    hc::accelerator_view acc_view = hc::accelerator().get_default_view(),
                    bool debug_synchronous = false)
{
    Counter * histogram_single[1] = { histogram };
    unsigned int levels_single[1] = { levels };
    Level lower_level_single[1] = { lower_level };
    Level upper_level_single[1] = { upper_level };

    detail::histogram_even_impl<1, 1>(
        temporary_storage, storage_size,
        samples, columns, rows, row_stride_bytes,
        histogram_single,
        levels_single, lower_level_single, upper_level_single,
        acc_view, debug_synchronous
    );
}

/// \brief Computes histograms from a sequence of multi-channel samples using equal-width bins.
///
/// \par
/// * The input is a sequence of <em>pixel</em> structures, where each pixel comprises
/// a record of \p Channels consecutive data samples (e.g., \p Channels = 4 for <em>RGBA</em> samples).
/// * The first \p ActiveChannels channels of total \p Channels channels will be used for computing histograms
/// (e.g., \p ActiveChannels = 3 for computing histograms of only <em>RGB</em> from <em>RGBA</em> samples).
/// * For channel<sub><em>i</em></sub> the number of histogram bins is (\p levels[i] - 1).
/// * For channel<sub><em>i</em></sub> bins are evenly-segmented and include the same width of sample values:
/// (\p upper_level[i] - \p lower_level[i]) / (\p levels[i] - 1).
/// * Returns the required size of \p temporary_storage in \p storage_size
/// if \p temporary_storage in a null pointer.
///
/// \tparam Channels - number of channels interleaved in the input samples.
/// \tparam ActiveChannels - number of channels being used for computing histograms.
/// \tparam SampleIterator - random-access iterator type of the input range. Must meet the
/// requirements of a C++ InputIterator concept. It can be a simple pointer type.
/// \tparam Counter - integer type for histogram bin counters.
/// \tparam Level - type of histogram boundaries (levels)
///
/// \param [in] temporary_storage - pointer to a device-accessible temporary storage. When
/// a null pointer is passed, the required allocation size (in bytes) is written to
/// \p storage_size and function returns without performing the reduction operation.
/// \param [in,out] storage_size - reference to a size (in bytes) of \p temporary_storage.
/// \param [in] samples - iterator to the first element in the range of input samples.
/// \param [in] size - number of pixels in the samples range.
/// \param [out] histogram - pointers to the first element in the histogram range, one for each active channel.
/// \param [in] levels - number of boundaries (levels) for histogram bins in each active channel.
/// \param [in] lower_level - lower sample value bound (inclusive) for the first histogram bin in each active channel.
/// \param [in] upper_level - upper sample value bound (exclusive) for the last histogram bin in each active channel.
/// \param [in] acc_view - [optional] \p hc::accelerator_view object. The default value
/// is \p hc::accelerator().get_default_view() (default view of the default accelerator).
/// \param [in] debug_synchronous - [optional] If true, synchronization after every kernel
/// launch is forced in order to check for errors. Default value is \p false.
///
/// \par Example
/// \parblock
/// In this example histograms for 3 channels (RGB) are computed on an array of 8-bit RGBA samples.
///
/// \code{.cpp}
/// #include <rocprim/rocprim.hpp>
///
/// hc::accelerator_view acc_view = ...;
///
/// // Prepare input and output (declare pointers, allocate device memory etc.)
/// unsigned int size;                 // e.g., 8
/// hc::array<unsigned char> samples;  // e.g., [(3, 1, 5, 255), (3, 1, 5, 255), (4, 2, 6, 127), (3, 2, 6, 127),
///                                    //        (0, 0, 0, 100), (0, 1, 0, 100), (0, 0, 1, 255), (0, 1, 1, 255)]
/// hc::array<int> histogram[3];       // 3 empty arrays of at least 256 elements each
/// unsigned int levels[3];            // e.g., [257, 257, 257] (for 256 bins)
/// int lower_level[3];                // e.g., [0, 0, 0]
/// int upper_level[3];                // e.g., [256, 256, 256]
///
/// int * histogram_ptr[3] = { histogram[0].accelerator_pointer(), histogram[1]... };
///
/// size_t temporary_storage_size_bytes;
/// // Get required size of the temporary storage
/// rocprim::multi_histogram_even<4, 3>(
///     nullptr, temporary_storage_size_bytes,
///     samples.accelerator_pointer(), size,
///     histogram_ptr, levels, lower_level, upper_level,
///     acc_view
/// );
///
/// // allocate temporary storage
/// hc::array<char> temporary_storage(temporary_storage_size_bytes, acc_view);
///
/// // compute histograms
/// rocprim::multi_histogram_even<4, 3>(
///     temporary_storage.accelerator_pointer(), temporary_storage_size_bytes,
///     samples.accelerator_pointer(), size,
///     histogram_ptr, levels, lower_level, upper_level,
///     acc_view
/// );
/// // histogram: [[4, 0, 0, 3, 1, 0, 0, ..., 0],
/// //             [2, 4, 2, 0, 0, 0, 0, ..., 0],
/// //             [2, 2, 0, 0, 0, 2, 2, ..., 0]]
/// \endcode
/// \endparblock
template<
    unsigned int Channels,
    unsigned int ActiveChannels,
    class SampleIterator,
    class Counter,
    class Level
>
inline
void multi_histogram_even(void * temporary_storage,
                          size_t& storage_size,
                          SampleIterator samples,
                          unsigned int size,
                          Counter * histogram[ActiveChannels],
                          unsigned int levels[ActiveChannels],
                          Level lower_level[ActiveChannels],
                          Level upper_level[ActiveChannels],
                          hc::accelerator_view acc_view = hc::accelerator().get_default_view(),
                          bool debug_synchronous = false)
{
    detail::histogram_even_impl<Channels, ActiveChannels>(
        temporary_storage, storage_size,
        samples, size, 1, 0,
        histogram,
        levels, lower_level, upper_level,
        acc_view, debug_synchronous
    );
}

/// \brief Computes histograms from a two-dimensional region of multi-channel samples using equal-width bins.
///
/// \par
/// * The two-dimensional region of interest within \p samples can be specified using the \p columns,
/// \p rows and \p row_stride_bytes parameters.
/// * The row stride must be a whole multiple of the sample data type size,
/// i.e., <tt>(row_stride_bytes % sizeof(std::iterator_traits<SampleIterator>::value_type)) == 0</tt>.
/// * The input is a sequence of <em>pixel</em> structures, where each pixel comprises
/// a record of \p Channels consecutive data samples (e.g., \p Channels = 4 for <em>RGBA</em> samples).
/// * The first \p ActiveChannels channels of total \p Channels channels will be used for computing histograms
/// (e.g., \p ActiveChannels = 3 for computing histograms of only <em>RGB</em> from <em>RGBA</em> samples).
/// * For channel<sub><em>i</em></sub> the number of histogram bins is (\p levels[i] - 1).
/// * For channel<sub><em>i</em></sub> bins are evenly-segmented and include the same width of sample values:
/// (\p upper_level[i] - \p lower_level[i]) / (\p levels[i] - 1).
/// * Returns the required size of \p temporary_storage in \p storage_size
/// if \p temporary_storage in a null pointer.
///
/// \tparam Channels - number of channels interleaved in the input samples.
/// \tparam ActiveChannels - number of channels being used for computing histograms.
/// \tparam SampleIterator - random-access iterator type of the input range. Must meet the
/// requirements of a C++ InputIterator concept. It can be a simple pointer type.
/// \tparam Counter - integer type for histogram bin counters.
/// \tparam Level - type of histogram boundaries (levels)
///
/// \param [in] temporary_storage - pointer to a device-accessible temporary storage. When
/// a null pointer is passed, the required allocation size (in bytes) is written to
/// \p storage_size and function returns without performing the reduction operation.
/// \param [in,out] storage_size - reference to a size (in bytes) of \p temporary_storage.
/// \param [in] samples - iterator to the first element in the range of input samples.
/// \param [in] columns - number of elements in each row of the region.
/// \param [in] rows - number of rows of the region.
/// \param [in] row_stride_bytes - number of bytes between starts of consecutive rows of the region.
/// \param [out] histogram - pointers to the first element in the histogram range, one for each active channel.
/// \param [in] levels - number of boundaries (levels) for histogram bins in each active channel.
/// \param [in] lower_level - lower sample value bound (inclusive) for the first histogram bin in each active channel.
/// \param [in] upper_level - upper sample value bound (exclusive) for the last histogram bin in each active channel.
/// \param [in] acc_view - [optional] \p hc::accelerator_view object. The default value
/// is \p hc::accelerator().get_default_view() (default view of the default accelerator).
/// \param [in] debug_synchronous - [optional] If true, synchronization after every kernel
/// launch is forced in order to check for errors. Default value is \p false.
///
/// \par Example
/// \parblock
/// In this example histograms for 3 channels (RGB) are computed on an array of 8-bit RGBA samples.
///
/// \code{.cpp}
/// #include <rocprim/rocprim.hpp>
///
/// hc::accelerator_view acc_view = ...;
///
/// // Prepare input and output (declare pointers, allocate device memory etc.)
/// unsigned int columns;             // e.g., 4
/// unsigned int rows;                // e.g., 2
/// size_t row_stride_bytes;          // e.g., 5 * sizeof(unsigned char)
/// hc::array<unsigned char> samples; // e.g., [(3, 1, 5, 0), (3, 1, 5, 0), (4, 2, 6, 0), (3, 2, 6, 0), (-, -, -, -),
///                                   //        (0, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 1, 1, 0), (-, -, -, -)]
/// hc::array<int> histogram[3];      // 3 empty arrays of at least 256 elements each
/// unsigned int levels[3];           // e.g., [257, 257, 257] (for 256 bins)
/// int lower_level[3];               // e.g., [0, 0, 0]
/// int upper_level[3];               // e.g., [256, 256, 256]
///
/// int * histogram_ptr[3] = { histogram[0].accelerator_pointer(), histogram[1]... };
///
/// size_t temporary_storage_size_bytes;
/// // Get required size of the temporary storage
/// rocprim::multi_histogram_even<4, 3>(
///     nullptr, temporary_storage_size_bytes,
///     samples.accelerator_pointer(), columns, rows, row_stride_bytes,
///     histogram_ptr, levels, lower_level, upper_level,
///     acc_view
/// );
///
/// // allocate temporary storage
/// hc::array<char> temporary_storage(temporary_storage_size_bytes, acc_view);
///
/// // compute histograms
/// rocprim::multi_histogram_even<4, 3>(
///     temporary_storage.accelerator_pointer(), temporary_storage_size_bytes,
///     samples.accelerator_pointer(), columns, rows, row_stride_bytes,
///     histogram_ptr, levels, lower_level, upper_level,
///     acc_view
/// );
/// // histogram: [[4, 0, 0, 3, 1, 0, 0, ..., 0],
/// //             [2, 4, 2, 0, 0, 0, 0, ..., 0],
/// //             [2, 2, 0, 0, 0, 2, 2, ..., 0]]
/// \endcode
/// \endparblock
template<
    unsigned int Channels,
    unsigned int ActiveChannels,
    class SampleIterator,
    class Counter,
    class Level
>
inline
void multi_histogram_even(void * temporary_storage,
                          size_t& storage_size,
                          SampleIterator samples,
                          unsigned int columns,
                          unsigned int rows,
                          size_t row_stride_bytes,
                          Counter * histogram[ActiveChannels],
                          unsigned int levels[ActiveChannels],
                          Level lower_level[ActiveChannels],
                          Level upper_level[ActiveChannels],
                          hc::accelerator_view acc_view = hc::accelerator().get_default_view(),
                          bool debug_synchronous = false)
{
    detail::histogram_even_impl<Channels, ActiveChannels>(
        temporary_storage, storage_size,
        samples, columns, rows, row_stride_bytes,
        histogram,
        levels, lower_level, upper_level,
        acc_view, debug_synchronous
    );
}

/// \brief Computes a histogram from a sequence of samples using the specified bin boundary levels.
///
/// \par
/// * The number of histogram bins is (\p levels - 1).
/// * The range for bin<sub><em>j</em></sub> is [<tt>level_values[j]</tt>, <tt>level_values[j+1]</tt>).
/// * Returns the required size of \p temporary_storage in \p storage_size
/// if \p temporary_storage in a null pointer.
///
/// \tparam SampleIterator - random-access iterator type of the input range. Must meet the
/// requirements of a C++ InputIterator concept. It can be a simple pointer type.
/// \tparam Counter - integer type for histogram bin counters.
/// \tparam Level - type of histogram boundaries (levels)
///
/// \param [in] temporary_storage - pointer to a device-accessible temporary storage. When
/// a null pointer is passed, the required allocation size (in bytes) is written to
/// \p storage_size and function returns without performing the reduction operation.
/// \param [in,out] storage_size - reference to a size (in bytes) of \p temporary_storage.
/// \param [in] samples - iterator to the first element in the range of input samples.
/// \param [in] size - number of elements in the samples range.
/// \param [out] histogram - pointer to the first element in the histogram range.
/// \param [in] levels - number of boundaries (levels) for histogram bins.
/// \param [in] level_values - pointer to the array of bin boundaries.
/// \param [in] acc_view - [optional] \p hc::accelerator_view object. The default value
/// is \p hc::accelerator().get_default_view() (default view of the default accelerator).
/// \param [in] debug_synchronous - [optional] If true, synchronization after every kernel
/// launch is forced in order to check for errors. Default value is \p false.
///
/// \par Example
/// \parblock
/// In this example a device-level histogram of 5 bins is computed on an array of float samples.
///
/// \code{.cpp}
/// #include <rocprim/rocprim.hpp>
///
/// hc::accelerator_view acc_view = ...;
///
/// // Prepare input and output (declare pointers, allocate device memory etc.)
/// unsigned int size;             // e.g., 8
/// hc::array<float> samples;      // e.g., [-10.0, 0.3, 9.5, 8.1, 1.5, 1.9, 100.0, 5.1]
/// hc::array<int> histogram;      // empty array of at least 5 elements
/// unsigned int levels;           // e.g., 6 (for 5 bins)
/// hc::array<float> level_values; // e.g., [0.0, 1.0, 5.0, 10.0, 20.0, 50.0]
///
/// size_t temporary_storage_size_bytes;
/// // Get required size of the temporary storage
/// rocprim::histogram_range(
///     nullptr, temporary_storage_size_bytes,
///     samples.accelerator_pointer(), size,
///     histogram.accelerator_pointer(), levels, level_values.accelerator_pointer(),
///     acc_view
/// );
///
/// // allocate temporary storage
/// hc::array<char> temporary_storage(temporary_storage_size_bytes, acc_view);
///
/// // compute histogram
/// rocprim::histogram_range(
///     temporary_storage.accelerator_pointer(), temporary_storage_size_bytes,
///     samples.accelerator_pointer(), size,
///     histogram.accelerator_pointer(), levels, level_values.accelerator_pointer(),
///     acc_view
/// );
/// // histogram: [1, 2, 3, 0, 0]
/// \endcode
/// \endparblock
template<
    class SampleIterator,
    class Counter,
    class Level
>
inline
void histogram_range(void * temporary_storage,
                     size_t& storage_size,
                     SampleIterator samples,
                     unsigned int size,
                     Counter * histogram,
                     unsigned int levels,
                     Level * level_values,
                     hc::accelerator_view acc_view = hc::accelerator().get_default_view(),
                     bool debug_synchronous = false)
{
    Counter * histogram_single[1] = { histogram };
    unsigned int levels_single[1] = { levels };
    Level * level_values_single[1] = { level_values };

    detail::histogram_range_impl<1, 1>(
        temporary_storage, storage_size,
        samples, size, 1, 0,
        histogram_single,
        levels_single, level_values_single,
        acc_view, debug_synchronous
    );
}

/// \brief Computes a histogram from a two-dimensional region of samples using the specified bin boundary levels.
///
/// \par
/// * The two-dimensional region of interest within \p samples can be specified using the \p columns,
/// \p rows and \p row_stride_bytes parameters.
/// * The row stride must be a whole multiple of the sample data type size,
/// i.e., <tt>(row_stride_bytes % sizeof(std::iterator_traits<SampleIterator>::value_type)) == 0</tt>.
/// * The number of histogram bins is (\p levels - 1).
/// * The range for bin<sub><em>j</em></sub> is [<tt>level_values[j]</tt>, <tt>level_values[j+1]</tt>).
/// * Returns the required size of \p temporary_storage in \p storage_size
/// if \p temporary_storage in a null pointer.
///
/// \tparam SampleIterator - random-access iterator type of the input range. Must meet the
/// requirements of a C++ InputIterator concept. It can be a simple pointer type.
/// \tparam Counter - integer type for histogram bin counters.
/// \tparam Level - type of histogram boundaries (levels)
///
/// \param [in] temporary_storage - pointer to a device-accessible temporary storage. When
/// a null pointer is passed, the required allocation size (in bytes) is written to
/// \p storage_size and function returns without performing the reduction operation.
/// \param [in,out] storage_size - reference to a size (in bytes) of \p temporary_storage.
/// \param [in] samples - iterator to the first element in the range of input samples.
/// \param [in] columns - number of elements in each row of the region.
/// \param [in] rows - number of rows of the region.
/// \param [in] row_stride_bytes - number of bytes between starts of consecutive rows of the region.
/// \param [out] histogram - pointer to the first element in the histogram range.
/// \param [in] levels - number of boundaries (levels) for histogram bins.
/// \param [in] level_values - pointer to the array of bin boundaries.
/// \param [in] acc_view - [optional] \p hc::accelerator_view object. The default value
/// is \p hc::accelerator().get_default_view() (default view of the default accelerator).
/// \param [in] debug_synchronous - [optional] If true, synchronization after every kernel
/// launch is forced in order to check for errors. Default value is \p false.
///
/// \par Example
/// \parblock
/// In this example a device-level histogram of 5 bins is computed on an array of float samples.
///
/// \code{.cpp}
/// #include <rocprim/rocprim.hpp>
///
/// hc::accelerator_view acc_view = ...;
///
/// // Prepare input and output (declare pointers, allocate device memory etc.)
/// unsigned int columns;          // e.g., 4
/// unsigned int rows;             // e.g., 2
/// size_t row_stride_bytes;       // e.g., 6 * sizeof(float)
/// hc::array<float> samples;      // e.g., [-10.0, 0.3, 9.5, 8.1, 1.5, 1.9, 100.0, 5.1]
/// hc::array<int> histogram;      // empty array of at least 5 elements
/// unsigned int levels;           // e.g., 6 (for 5 bins)
/// hc::array<float> level_values; // e.g., [0.0, 1.0, 5.0, 10.0, 20.0, 50.0]
///
/// size_t temporary_storage_size_bytes;
/// // Get required size of the temporary storage
/// rocprim::histogram_range(
///     nullptr, temporary_storage_size_bytes,
///     samples.accelerator_pointer(), columns, rows, row_stride_bytes,
///     histogram.accelerator_pointer(), levels, level_values.accelerator_pointer(),
///     acc_view
/// );
///
/// // allocate temporary storage
/// hc::array<char> temporary_storage(temporary_storage_size_bytes, acc_view);
///
/// // compute histogram
/// rocprim::histogram_range(
///     temporary_storage.accelerator_pointer(), temporary_storage_size_bytes,
///     samples.accelerator_pointer(), columns, rows, row_stride_bytes,
///     histogram.accelerator_pointer(), levels, level_values.accelerator_pointer(),
///     acc_view
/// );
/// // histogram: [1, 2, 3, 0, 0]
/// \endcode
/// \endparblock
template<
    class SampleIterator,
    class Counter,
    class Level
>
inline
void histogram_range(void * temporary_storage,
                     size_t& storage_size,
                     SampleIterator samples,
                     unsigned int columns,
                     unsigned int rows,
                     size_t row_stride_bytes,
                     Counter * histogram,
                     unsigned int levels,
                     Level * level_values,
                     hc::accelerator_view acc_view = hc::accelerator().get_default_view(),
                     bool debug_synchronous = false)
{
    Counter * histogram_single[1] = { histogram };
    unsigned int levels_single[1] = { levels };
    Level * level_values_single[1] = { level_values };

    detail::histogram_range_impl<1, 1>(
        temporary_storage, storage_size,
        samples, columns, rows, row_stride_bytes,
        histogram_single,
        levels_single, level_values_single,
        acc_view, debug_synchronous
    );
}

/// \brief Computes histograms from a sequence of multi-channel samples using the specified bin boundary levels.
///
/// \par
/// * The input is a sequence of <em>pixel</em> structures, where each pixel comprises
/// a record of \p Channels consecutive data samples (e.g., \p Channels = 4 for <em>RGBA</em> samples).
/// * The first \p ActiveChannels channels of total \p Channels channels will be used for computing histograms
/// (e.g., \p ActiveChannels = 3 for computing histograms of only <em>RGB</em> from <em>RGBA</em> samples).
/// * For channel<sub><em>i</em></sub> the number of histogram bins is (\p levels[i] - 1).
/// * For channel<sub><em>i</em></sub> the range for bin<sub><em>j</em></sub> is
/// [<tt>level_values[i][j]</tt>, <tt>level_values[i][j+1]</tt>).
/// * Returns the required size of \p temporary_storage in \p storage_size
/// if \p temporary_storage in a null pointer.
///
/// \tparam Channels - number of channels interleaved in the input samples.
/// \tparam ActiveChannels - number of channels being used for computing histograms.
/// \tparam SampleIterator - random-access iterator type of the input range. Must meet the
/// requirements of a C++ InputIterator concept. It can be a simple pointer type.
/// \tparam Counter - integer type for histogram bin counters.
/// \tparam Level - type of histogram boundaries (levels)
///
/// \param [in] temporary_storage - pointer to a device-accessible temporary storage. When
/// a null pointer is passed, the required allocation size (in bytes) is written to
/// \p storage_size and function returns without performing the reduction operation.
/// \param [in,out] storage_size - reference to a size (in bytes) of \p temporary_storage.
/// \param [in] samples - iterator to the first element in the range of input samples.
/// \param [in] size - number of pixels in the samples range.
/// \param [out] histogram - pointers to the first element in the histogram range, one for each active channel.
/// \param [in] levels - number of boundaries (levels) for histogram bins in each active channel.
/// \param [in] level_values - pointer to the array of bin boundaries for each active channel.
/// \param [in] acc_view - [optional] \p hc::accelerator_view object. The default value
/// is \p hc::accelerator().get_default_view() (default view of the default accelerator).
/// \param [in] debug_synchronous - [optional] If true, synchronization after every kernel
/// launch is forced in order to check for errors. Default value is \p false.
///
/// \par Example
/// \parblock
/// In this example histograms for 3 channels (RGB) are computed on an array of 8-bit RGBA samples.
///
/// \code{.cpp}
/// #include <rocprim/rocprim.hpp>
///
/// hc::accelerator_view acc_view = ...;
///
/// // Prepare input and output (declare pointers, allocate device memory etc.)
/// unsigned int size;                // e.g., 8
/// hc::array<unsigned char> samples; // e.g., [(0, 0, 80, 255), (120, 0, 80, 255), (123, 0, 82, 127), (10, 1, 83, 0),
///                                   //        (51, 1, 8, 100), (52, 1, 8, 100), (53, 0, 81, 255), (54, 50, 81, 255)]
/// hc::array<int> histogram[3];      // 3 empty arrays of at least 256 elements each
/// unsigned int levels[3];           // e.g., [4, 4, 3]
/// hc::array<int> level_values[3];   // e.g., [[0, 50, 100, 200], [0, 20, 40, 60], [0, 10, 100]]
///
/// int * histogram_ptr[3] = { histogram[0].accelerator_pointer(), histogram[1]... };
/// int * level_values_ptr[3] = { level_values[0].accelerator_pointer(), level_values[1]... };
///
/// size_t temporary_storage_size_bytes;
/// // Get required size of the temporary storage
/// rocprim::multi_histogram_range<4, 3>(
///     nullptr, temporary_storage_size_bytes,
///     samples.accelerator_pointer(), size,
///     histogram_ptr, levels, level_values_ptr,
///     acc_view
/// );
///
/// // allocate temporary storage
/// hc::array<char> temporary_storage(temporary_storage_size_bytes, acc_view);
///
/// // compute histograms
/// rocprim::multi_histogram_range<4, 3>(
///     temporary_storage.accelerator_pointer(), temporary_storage_size_bytes,
///     samples.accelerator_pointer(), size,
///     histogram_ptr, levels, level_values_ptr,
///     acc_view
/// );
/// // histogram: [[2, 4, 2], [7, 0, 1], [2, 6]]
/// \endcode
/// \endparblock
template<
    unsigned int Channels,
    unsigned int ActiveChannels,
    class SampleIterator,
    class Counter,
    class Level
>
inline
void multi_histogram_range(void * temporary_storage,
                           size_t& storage_size,
                           SampleIterator samples,
                           unsigned int size,
                           Counter * histogram[ActiveChannels],
                           unsigned int levels[ActiveChannels],
                           Level * level_values[ActiveChannels],
                           hc::accelerator_view acc_view = hc::accelerator().get_default_view(),
                           bool debug_synchronous = false)
{
    detail::histogram_range_impl<Channels, ActiveChannels>(
        temporary_storage, storage_size,
        samples, size, 1, 0,
        histogram,
        levels, level_values,
        acc_view, debug_synchronous
    );
}

/// \brief Computes histograms from a two-dimensional region of multi-channel samples using the specified bin
/// boundary levels.
///
/// \par
/// * The two-dimensional region of interest within \p samples can be specified using the \p columns,
/// \p rows and \p row_stride_bytes parameters.
/// * The row stride must be a whole multiple of the sample data type size,
/// i.e., <tt>(row_stride_bytes % sizeof(std::iterator_traits<SampleIterator>::value_type)) == 0</tt>.
/// * The input is a sequence of <em>pixel</em> structures, where each pixel comprises
/// a record of \p Channels consecutive data samples (e.g., \p Channels = 4 for <em>RGBA</em> samples).
/// * The first \p ActiveChannels channels of total \p Channels channels will be used for computing histograms
/// (e.g., \p ActiveChannels = 3 for computing histograms of only <em>RGB</em> from <em>RGBA</em> samples).
/// * For channel<sub><em>i</em></sub> the number of histogram bins is (\p levels[i] - 1).
/// * For channel<sub><em>i</em></sub> the range for bin<sub><em>j</em></sub> is
/// [<tt>level_values[i][j]</tt>, <tt>level_values[i][j+1]</tt>).
/// * Returns the required size of \p temporary_storage in \p storage_size
/// if \p temporary_storage in a null pointer.
///
/// \tparam Channels - number of channels interleaved in the input samples.
/// \tparam ActiveChannels - number of channels being used for computing histograms.
/// \tparam SampleIterator - random-access iterator type of the input range. Must meet the
/// requirements of a C++ InputIterator concept. It can be a simple pointer type.
/// \tparam Counter - integer type for histogram bin counters.
/// \tparam Level - type of histogram boundaries (levels)
///
/// \param [in] temporary_storage - pointer to a device-accessible temporary storage. When
/// a null pointer is passed, the required allocation size (in bytes) is written to
/// \p storage_size and function returns without performing the reduction operation.
/// \param [in,out] storage_size - reference to a size (in bytes) of \p temporary_storage.
/// \param [in] samples - iterator to the first element in the range of input samples.
/// \param [in] columns - number of elements in each row of the region.
/// \param [in] rows - number of rows of the region.
/// \param [in] row_stride_bytes - number of bytes between starts of consecutive rows of the region.
/// \param [out] histogram - pointers to the first element in the histogram range, one for each active channel.
/// \param [in] levels - number of boundaries (levels) for histogram bins in each active channel.
/// \param [in] level_values - pointer to the array of bin boundaries for each active channel.
/// \param [in] acc_view - [optional] \p hc::accelerator_view object. The default value
/// is \p hc::accelerator().get_default_view() (default view of the default accelerator).
/// \param [in] debug_synchronous - [optional] If true, synchronization after every kernel
/// launch is forced in order to check for errors. Default value is \p false.
///
/// \par Example
/// \parblock
/// In this example histograms for 3 channels (RGB) are computed on an array of 8-bit RGBA samples.
///
/// \code{.cpp}
/// #include <rocprim/rocprim.hpp>
///
/// hc::accelerator_view acc_view = ...;
///
/// // Prepare input and output (declare pointers, allocate device memory etc.)
/// unsigned int columns;           // e.g., 4
/// unsigned int rows;              // e.g., 2
/// size_t row_stride_bytes;        // e.g., 5 * sizeof(unsigned char)
/// hc::array<unsigned char> samples;
///                         // e.g., [(0, 0, 80, 0), (120, 0, 80, 0), (123, 0, 82, 0), (10, 1, 83, 0), (-, -, -, -),
///                         //        (51, 1, 8, 0), (52, 1, 8, 0), (53, 0, 81, 0), (54, 50, 81, 0), (-, -, -, -)]
/// hc::array<int> histogram[3];    // 3 empty arrays
/// unsigned int levels[3];         // e.g., [4, 4, 3]
/// hc::array<int> level_values[3]; // e.g., [[0, 50, 100, 200], [0, 20, 40, 60], [0, 10, 100]]
///
/// int * histogram_ptr[3] = { histogram[0].accelerator_pointer(), histogram[1]... };
/// int * level_values_ptr[3] = { level_values[0].accelerator_pointer(), level_values[1]... };
///
/// size_t temporary_storage_size_bytes;
/// // Get required size of the temporary storage
/// rocprim::multi_histogram_range<4, 3>(
///     nullptr, temporary_storage_size_bytes,
///     samples.accelerator_pointer(), columns, rows, row_stride_bytes,
///     histogram_ptr, levels, level_values_ptr,
///     acc_view
/// );
///
/// // allocate temporary storage
/// hc::array<char> temporary_storage(temporary_storage_size_bytes, acc_view);
///
/// // compute histograms
/// rocprim::multi_histogram_range<4, 3>(
///     temporary_storage.accelerator_pointer(), temporary_storage_size_bytes,
///     samples.accelerator_pointer(), columns, rows, row_stride_bytes,
///     histogram_ptr, levels, level_values_ptr,
///     acc_view
/// );
/// // histogram: [[2, 4, 2], [7, 0, 1], [2, 6]]
/// \endcode
/// \endparblock
template<
    unsigned int Channels,
    unsigned int ActiveChannels,
    class SampleIterator,
    class Counter,
    class Level
>
inline
void multi_histogram_range(void * temporary_storage,
                           size_t& storage_size,
                           SampleIterator samples,
                           unsigned int columns,
                           unsigned int rows,
                           size_t row_stride_bytes,
                           Counter * histogram[ActiveChannels],
                           unsigned int levels[ActiveChannels],
                           Level * level_values[ActiveChannels],
                           hc::accelerator_view acc_view = hc::accelerator().get_default_view(),
                           bool debug_synchronous = false)
{
    detail::histogram_range_impl<Channels, ActiveChannels>(
        temporary_storage, storage_size,
        samples, columns, rows, row_stride_bytes,
        histogram,
        levels, level_values,
        acc_view, debug_synchronous
    );
}

/// @}
// end of group devicemodule_hc

END_ROCPRIM_NAMESPACE

#endif // ROCPRIM_DEVICE_DEVICE_HISTOGRAM_HC_HPP_