
#ifndef SPECTRALPROCESSOR_H
#define SPECTRALPROCESSOR_H

#include <algorithm>
#include "HISSTools_FFT/HISSTools_FFT.h"
#include "HISSTools_IR_Manipulation/HISSTools_IR_Manipulation.h"

enum EdgeMode { kEdgeLinear, kEdgeWrap, kEdgeWrapCentre, kEdgeFold };

template <typename T, typename Allocator>
class spectral_processor
{
    struct Sizes
    {
        Sizes(uintptr_t size1, uintptr_t size2)
        : m_size1(size1), m_size2(size2), m_fft_size_log2(calc_fft_size_log2(linear())) {}
        
        uintptr_t linear() const        { return m_size1 + m_size2 - 1; }
        uintptr_t min() const           { return std::min(m_size1, m_size2); }
        uintptr_t max() const           { return std::max(m_size1, m_size2); }
        uintptr_t fft() const           { return 1 << m_fft_size_log2; }
        uintptr_t fft_log2() const      { return m_fft_size_log2; }
        
    private:
        
        uintptr_t m_size1, m_size2, m_fft_size_log2;
    };
    
    using SplitType = typename FFTTypes<T>::SplitType;
    using SetupType = typename FFTTypes<T>::SetupType;

    typedef void (*SpectralOp)(SplitType *, SplitType *, SplitType *, uintptr_t, T);
    typedef void (*ComplexArrange)(SplitType, SplitType, const Sizes&, EdgeMode);
    typedef void (*RealArrange)(T *, SplitType, const Sizes&, EdgeMode);

public:
    
    struct sized_in
    {
        sized_in(const T* ptr, uintptr_t size) : m_ptr(ptr), m_size(size) {}
        
        const T* m_ptr;
        const uintptr_t m_size;
    };
    
    // Constructor
    
    spectral_processor(Allocator allocator) : m_allocator(allocator), m_max_fft_size_log2(0)
    {
        set_max_fft_size(32768);
    }
    
    ~spectral_processor()
    {
        if (m_max_fft_size_log2)
            hisstools_destroy_setup(m_fft_setup);
    }
    
    void set_max_fft_size(uintptr_t size)
    {
        uintptr_t fft_size_log2 = calc_fft_size_log2(size);
        
        if (fft_size_log2 != m_max_fft_size_log2)
        {
            if (m_max_fft_size_log2)
                hisstools_destroy_setup(m_fft_setup);
            if (fft_size_log2)
                hisstools_create_setup(&m_fft_setup, fft_size_log2);
            m_max_fft_size_log2 = fft_size_log2;
        }
    }
    
    uintptr_t max_fft_size() const { return 1 << m_max_fft_size_log2; }
    
    // Transforms
    
    void fft(SplitType& io, uintptr_t fft_size_log2)
    {
        hisstools_fft(m_fft_setup, &io, fft_size_log2);
    }
    
    void rfft(SplitType& io, uintptr_t fft_size_log2)
    {
        hisstools_rfft(m_fft_setup, &io, fft_size_log2);
    }
    
    void rfft(SplitType& output, const T *input, uintptr_t size, uintptr_t fft_size_log2)
    {
        hisstools_rfft(m_fft_setup, input, &output, size, fft_size_log2);
    }
    
    void ifft(SplitType& io, uintptr_t fft_size_log2)
    {
        hisstools_ifft(m_fft_setup, &io, fft_size_log2);
    }
    
    void rifft(SplitType& io, uintptr_t fft_size_log2)
    {
        hisstools_rifft(m_fft_setup, &io, fft_size_log2);
    }
    
    void rifft(T *output, SplitType& input, uintptr_t fft_size_log2)
    {
        hisstools_rifft(m_fft_setup, &input, output, fft_size_log2);
    }
    
    // Convolution
    
    void convolve_complex(T *r_out, T *i_out, sized_in r_in1, sized_in i_in1, sized_in r_in2, sized_in i_in2, EdgeMode mode)
    {
        binary_op<ir_convolve_complex, arrange_convolve<SplitType>>(r_out, i_out, r_in1, i_in1, r_in2, i_in2, mode);
    }
    
    void convolve_real(T *output, sized_in in1, sized_in in2, EdgeMode mode)
    {
        binary_op<ir_convolve_real, arrange_convolve<T*>>(output, in1, in2, mode);
    }
    
    // Correlation
    
    void correlate_complex(T *r_out, T *i_out, sized_in r_in1, sized_in i_in1, sized_in r_in2, sized_in i_in2, EdgeMode mode)
    {
        binary_op<ir_correlate_complex, arrange_correlate<SplitType>>(r_out, i_out, r_in1, i_in1, r_in2, i_in2, mode);
    }
    
    void correlate_real(T *output, sized_in in1, sized_in in2, EdgeMode mode)
    {
        binary_op<&ir_correlate_real, &arrange_correlate<T*>>(output, in1, in2, mode);
    }
    
    // Phase
    
    void change_phase(T *output, const T *input, uintptr_t size, double phase)
    {
        uintptr_t fft_size_log2 = calc_fft_size_log2(size);
        uintptr_t fft_size = 1 << fft_size_log2;
        
        temporary_buffers<1> buffer(m_allocator, fft_size >> 1);
        
        rfft(buffer.mSpectra[0], input, size, fft_size_log2);
        ir_phase(m_fft_setup, &buffer.mSpectra[0], &buffer.mSpectra[0], fft_size, phase);
        rifft(output, buffer.mSpectra[0], fft_size_log2);
        
        scaleVector(output, fft_size, T(0.5) / (T) fft_size);
    }
    
    uintptr_t calc_convolved_size(uintptr_t size1, uintptr_t size2, EdgeMode mode) const
    {
        return calc_conv_corr_size(size1, size2, mode);
    }
    
    uintptr_t calc_correlated_size(uintptr_t size1, uintptr_t size2, EdgeMode mode) const
    {
        return calc_conv_corr_size(size1, size2, mode);
    }
    
    static uintptr_t calc_fft_size_log2(uintptr_t size)
    {
        uintptr_t bit_shift = size;
        uintptr_t bit_count = 0;
        
        while (bit_shift)
        {
            bit_shift >>= 1U;
            bit_count++;
        }
        
        if (bit_count && size == 1U << (bit_count - 1U))
            return bit_count - 1U;
        else
            return bit_count;
    }
    
    // Scale Vector
    
    void scale_vector(T *io, uintptr_t size, T scale)
    {
        if (scale == 1.0)
            return;
        
        for (uintptr_t i = 0; i < size; i++)
            io[i] *= scale;
    }
    
    // Scale Spectrum
    
    void scale_spectrum(SplitType &io, uintptr_t size, T scale)
    {
        if (scale == 1.0)
            return;
        
        for (uintptr_t i = 0; i < size; i++)
        {
            io.realp[i] *= scale;
            io.imagp[i] *= scale;
        }
    }
    
private:
    
    // Temporary Memory
    
    template <int N>
    struct temporary_buffers
    {
        temporary_buffers(Allocator& allocator, uintptr_t size) : m_allocator(allocator)
        {
            T* ptr = m_allocator.template allocate<T>(size * 2 * N * sizeof(T));
            
            for (int i = 0; i < N; i++)
            {
                m_spectra[i].realp = ptr + (N * 2 * size);
                m_spectra[i].imagp = ptr + ((N * 2 + 1) * size);
            }
        }
        
        temporary_buffers(const temporary_buffers&) = delete;
        temporary_buffers & operator=(const temporary_buffers&) = delete;
        
        ~temporary_buffers() { m_allocator.deallocate(m_spectra[0].realp); }
        
        operator bool() { return m_spectra[0].realp; }
        
        Allocator &m_allocator;
        SplitType m_spectra[N];
    };
    
    struct zipped_pointer
    {
        zipped_pointer(const SplitType spectrum, uintptr_t offset)
        : p1(spectrum.realp + (offset >> 1)), p2(spectrum.imagp + (offset >> 1))
        {
            if (offset & 1U)
                (*this)++;
        }
        
        const T *operator ++(int)
        {
            const T *p = p1;
            std::swap(++p1, p2);
            return p;
        }
        
        const T *operator --(int)
        {
            const T *p = p1;
            std::swap(p1, --p2);
            return p;
        }
        
    private:
        
        const T *p1, *p2;
    };
    
    // Memory manipulation (complex)
    
    static void copy_zero(T* output, sized_in in, uintptr_t size)
    {
        std::copy_n(in.m_ptr, in.m_size, output);
        std::fill_n(output + in.m_size, size - in.m_size, 0.0);
    }
    
    static void copy(SplitType& output, const SplitType& spectrum, uintptr_t oOffset, uintptr_t offset, uintptr_t size)
    {
        std::copy_n(spectrum.realp + offset, size, output.realp + oOffset);
        std::copy_n(spectrum.imagp + offset, size, output.imagp + oOffset);
    }
    
    static void wrap(SplitType& output, const SplitType& spectrum, uintptr_t oOffset, uintptr_t offset, uintptr_t size)
    {
        for (uintptr_t i = 0; i < size; i++)
        {
            output.realp[i + oOffset] += spectrum.realp[i + offset];
            output.imagp[i + oOffset] += spectrum.imagp[i + offset];
        }
    }
    
    static void fold(SplitType& output, const SplitType& spectrum, uintptr_t oOffset, uintptr_t offset, uintptr_t size)
    {
        for (uintptr_t i = 0; i < size; i++)
        {
            output.realp[i + oOffset] += spectrum.realp[offset - (i + 1)];
            output.imagp[i + oOffset] += spectrum.imagp[offset - (i + 1)];
        }
    }

    // Memory manipulation (real)
    
    static void copy(T *output, const SplitType& spectrum, uintptr_t oOffset, uintptr_t offset, uintptr_t size)
    {
        zipped_pointer p(spectrum, offset);

        for (uintptr_t i = 0; i < size; i++)
            output[oOffset + i] = *p++;
    }
    
    static void wrap(T *output, const SplitType& spectrum, uintptr_t oOffset, uintptr_t offset, uintptr_t size)
    {
        zipped_pointer p(spectrum, offset);
        
        for (uintptr_t i = 0; i < size; i++)
            output[oOffset + i] += *p++;
    }
    
    static void fold(T *output, const SplitType& spectrum, uintptr_t oOffset, uintptr_t offset, uintptr_t size)
    {
        zipped_pointer p(spectrum, offset);
        
        for (uintptr_t i = 0; i < size; i++)
            output[oOffset + i] += *p--;
    }
    
    // Arranges for convolution and correlation
    
    template <class U>
    static void arrange_convolve(U output, SplitType spectrum, const Sizes& sizes, EdgeMode mode)
    {
        uintptr_t folded = sizes.min() / 2;
        uintptr_t wrapped1 = (sizes.min() - 1) / 2;
        uintptr_t wrapped2 = (sizes.min() - 1) - wrapped1;
        
        switch (mode)
        {
            case kEdgeLinear:
                copy(output, spectrum, 0, 0, sizes.linear());
                break;

            case kEdgeWrap:
                copy(output, spectrum, 0, 0, sizes.max());
                wrap(output, spectrum, 0, sizes.max(), sizes.linear() - sizes.max());
                break;
        
            case kEdgeWrapCentre:
                copy(output, spectrum, 0, wrapped1, sizes.max());
                wrap(output, spectrum, 0, sizes.linear() - wrapped2, wrapped2);
                wrap(output, spectrum, sizes.max() - wrapped1, 0, wrapped1);
                break;
                
            case kEdgeFold:
                copy(output, spectrum, 0, (sizes.min() - 1) / 2, sizes.max());
                fold(output, spectrum, 0, folded, folded);
                fold(output, spectrum, sizes.max() - folded, sizes.linear(), folded);
                break;
        }
    }

    template <class U>
    static void arrange_correlate(U output, SplitType spectrum, const Sizes& sizes, EdgeMode mode)
    {
        uintptr_t offset = sizes.min() / 2;
        uintptr_t wrapped = sizes.min() - offset - 1;
        
        switch (mode)
        {
            case kEdgeLinear:
                copy(output, spectrum, 0, 0, sizes.max());
                copy(output, spectrum, sizes.max(), sizes.fft() + 1 - sizes.min(), sizes.min() - 1);
                break;
                
            case kEdgeWrap:
                copy(output, spectrum, 0, 0, sizes.max());
                wrap(output, spectrum, sizes.linear() + 1 - (2 * sizes.min()), sizes.fft() + 1 - sizes.min(), sizes.min() - 1);
                break;
                
            case kEdgeWrapCentre:
                copy(output, spectrum, 0, sizes.fft() - offset, offset);
                copy(output, spectrum, offset, 0, sizes.max() - offset);
                wrap(output, spectrum, 0, sizes.max() - offset, offset);
                wrap(output, spectrum, sizes.max() - wrapped, sizes.fft() + 1 - sizes.min(), wrapped);
                break;
            
            case kEdgeFold:
                copy(output, spectrum, 0, sizes.fft() - offset, offset);
                copy(output, spectrum, offset, 0, sizes.max() - offset);
                fold(output, spectrum, 0, sizes.fft() + 1 - offset, offset);
                fold(output, spectrum, sizes.max() - offset, sizes.max(), offset);
                break;
        }
    }
   
    // Binary Operations
    
    uintptr_t calc_conv_corr_size(uintptr_t size1, uintptr_t size2, EdgeMode mode) const
    {
        Sizes sizes(size1, size2);
        
        if ((sizes.fft() > max_fft_size()) || !size1 || !size2)
            return 0;
        
        uintptr_t size = mode != kEdgeLinear ? sizes.max() : sizes.linear();
        
        if (mode == kEdgeFold && !(sizes.min() & 1U))
            return size + 1;
        
        return size;
    }
    
    template<SpectralOp Op, ComplexArrange arrange>
    void binary_op(T *r_out, T *i_out, sized_in r_in1, sized_in i_in1, sized_in r_in2, sized_in i_in2, EdgeMode mode)
    {
        uintptr_t size1 = std::max(r_in1.m_size, i_in1.m_size);
        uintptr_t size2 = std::max(r_in2.m_size, i_in2.m_size);
        
        if (!calc_conv_corr_size(size1, size2, mode))
            return;
        
        // Special case for single sample inputs

        if (size1 == 1 && size2 == 1)
        {
            r_out[0] = r_in1.m_ptr[0] * r_in2.m_ptr[0] + i_in1.m_ptr[0] * i_in2.m_ptr[0];
            i_out[0] = r_in1.m_ptr[0] * i_in2.m_ptr[0] - i_in1.m_ptr[0] * i_in1.m_ptr[0];
            return;
        }
        
        // Assign temporary memory
        
        Sizes sizes(size1, size2);
        temporary_buffers<2> buffers(m_allocator, sizes.fft());
        SplitType output {r_out, i_out};
        
        // Process
        
        if (buffers)
        {
            copy_zero(buffers.m_spectra[0].realp, r_in1, sizes.fft());
            copy_zero(buffers.m_spectra[0].imagp, i_in1, sizes.fft());
            copy_zero(buffers.m_spectra[1].realp, r_in2, sizes.fft());
            copy_zero(buffers.m_spectra[1].imagp, i_in2, sizes.fft());
            
            fft(buffers.m_spectra[0], sizes.fft_log2());
            fft(buffers.m_spectra[1], sizes.fft_log2());
            
            Op(&buffers.m_spectra[0], &buffers.m_spectra[0], &buffers.m_spectra[1], sizes.fft(), 1.0 / (T) sizes.fft());
            
            ifft(buffers.m_spectra[0], sizes.fft_log2());
            
            arrange(output, buffers.m_spectra[0], sizes, mode);
        }
    }
    
    template<SpectralOp Op, RealArrange arrange>
    void binary_op(T *output, sized_in in1, sized_in in2, EdgeMode mode)
    {
        if (!calc_conv_corr_size(in1.m_size, in2.m_size, mode))
            return;
        
        // Special case for single sample inputs

        if (in1.m_size == 1 && in2.m_size == 1)
        {
            output[0] = in1.m_ptr[0] * in2.m_ptr[0];
            return;
        }
        
        // Assign temporary memory
        
        Sizes sizes(in1.m_size, in2.m_size);
        temporary_buffers<2> buffers(m_allocator, sizes.fft() >> 1);
        
        // Process
        
        if (buffers)
        {
            rfft(buffers.m_spectra[0], in1.m_ptr, in1.m_size, sizes.fft_log2());
            rfft(buffers.m_spectra[1], in2.m_ptr, in2.m_size, sizes.fft_log2());
            
            Op(&buffers.m_spectra[0], &buffers.m_spectra[0], &buffers.m_spectra[1], sizes.fft() >> 1, 0.25 / (T) sizes.fft());
            
            rifft(buffers.m_spectra[0], sizes.fft_log2());
            
            arrange(output, buffers.m_spectra[0], sizes, mode);
        }
    }
    
    // Data
    
    Allocator m_allocator;
    SetupType m_fft_setup;
    uintptr_t m_max_fft_size_log2;
};

#endif
