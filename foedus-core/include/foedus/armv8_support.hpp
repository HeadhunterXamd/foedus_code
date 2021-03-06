/*
 * Copyright (c) 2014-2015, Hewlett-Packard Development Company, LP.
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details. You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * HP designates this particular file as subject to the "Classpath" exception
 * as provided by HP in the LICENSE.txt file that accompanied this code.
 */
#ifndef FOEDUS_ARMV8_SUPPORT_HPP_
#define FOEDUS_ARMV8_SUPPORT_HPP_

/**
 * @defgroup ARMV8 Memorandum on ARMv8 (AArch64) Support
 * @ingroup COMPILER
 * @brief We keep notes on FOEDUS's ARMv8 (AArch64) Support here.
 * @details
 * This file is not a source code. It's just a collection of notes on
 * what we did and what we have to keep in mind regarding ARMv8 support.
 * It's an emerging environment where some information that was correct quickly become obsolete.
 * So, also leave the date you wrote each section.
 *
 * @par Target compiler is GCC/clang
 * We assume gcc/clang. No ICC. We initially assumed gcc only, but
 * we plesantly found that clang has a quite good gcc-compatibility.
 * So, we got FOEDUS work on clang without much hassle.
 * We aggressively use gcc's builtin methods to abstract x86-aarch64 differences
 * as far as they are supported in clang, and most of them are!
 *
 * Sorry, ICC, you seem to be a red-headed stepchild.
 *
 * @par __aarch64__ and __AARCH64EB__/__AARCH64EL__ macro
 * This macro is defined if gcc is running on AArch64 (latter two for big/little endian, but
 * I bet you are getting __AARCH64EL__. see "gcc -dM -E - < /dev/null ").
 * Use it like:
 * @code{.cpp}
 * #if defined(__aarch64__)
 * ...
 * #if undefined(__aarch64__)
 * ...
 * @endcode
 *
 * @par 128-bit atomic CAS
 * [Dec14]
 * So many gotcha's about this.
 * First, gcc on x86 allows users to specify "-mcx16" to enable __sync_bool_compare_and_swap on
 * __uint128_t, __atomic_compare_exchange_16, etc.
 * ARMv8 does support 128-bit atomic operations such as ldaxp/stlxp, but gcc-AArch64 doesn't support
 * -mcx16. We initially thought this means we can't do cas128 without resorting to assembly.
 * However, it turns out that gcc-AArch64 does allow __atomic_compare_exchange_16 if one links to
 * \b libatomic.so, which is a shared library that is a part of newer gcc.
 * This library is provided in gcc-AArch64. See our CMakeLists.txt and FindGccAtomic.cmake for
 * more details. We keep using the old way (-mcx16 without libatomic.so) in x86 because that would
 * be just a waste (one shared-library call overhead) on x86.
 *
 * @par Atomic CAS on ARMV8.1
 * [Dec14] There is an additional information on this subject.
 * Currently, ARMv8 doesn't have a specific instruction for CAS (like x86's cmpxchg).
 * You lock the cacheline with ldax, then store with stlx.
 * Some one says that this might be different in ARMv8.1, adding cmpxchg for better performance.
 * It makes sense and might be much faster. We hope gcc would automatically make use of it.
 *
 * @par [...]mintrin.h, such as xmmintrin.h
 * [Dec14]
 * In one sentence, it's not there.
 * /usr/lib/gcc/aarch64-linux-gnu/4.8.2/include contains a surprisingly lower number of files
 * than /usr/lib/gcc/x86_64-redhat-linux/4.8.3/include/, and all mintrin.h are gone.
 * We must not depend on them, or use ifdef for AArch64.
 * It makes sense because these files are mainly for x86's SSE. But it did contain many other things
 * such as mm_pause and mm_prefetch.
 *
 * @par Cacheline prefetch
 * [Dec14]
 * Because xmmintrin.h is not available, we use gcc's __builtin_prefetch on ARMv8 instead of
 * mm_prefetch, discarding compiler-portability for the sake of OS-portability. how damn.
 * __builtin_prefetch also allows users to specify r or rw, but as far as I understand no
 * implementation actually makes use of this hint so far.
 *
 * @par RDTSC-equivalent
 * [Dec14]
 * foedus/debugging/rdtsc.hpp uses x86's rdtsc as a low-overhead high-precision counter.
 * The equivalent on ARMv8 is cntvct_el0, which can be used in user mode unlike earlier ARM ISA.
 */

#endif  // FOEDUS_ARMV8_SUPPORT_HPP_
