/*
 * FileIdentifier.h
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once
#include <cstdint>
#include <type_traits>

using FileIdentifier = uint32_t;

struct Empty {};

template <typename T, typename = int>
struct HasFileIdentifierMember : std::false_type {};

template <typename T>
struct HasFileIdentifierMember<T, decltype((void)T::file_identifier, 0)> : std::true_type {};

template <class T, bool>
struct FileIdentifierForBase;

template <class T>
struct FileIdentifierForBase<T, false> {};

template <class T>
struct FileIdentifierForBase<T, true> {
	static constexpr FileIdentifier value = T::file_identifier;
};

template <class T>
struct FileIdentifierFor : FileIdentifierForBase<T, HasFileIdentifierMember<T>::value> {};

template <typename T, typename = int>
struct HasFileIdentifier : std::false_type {};

template <typename T>
struct HasFileIdentifier<T, decltype((void)FileIdentifierFor<T>::value, 0)> : std::true_type {};

template <class T, uint32_t B, bool = HasFileIdentifier<T>::value>
struct ComposedIdentifier;

template <class T, uint32_t B>
struct ComposedIdentifier<T, B, true>
{
	static constexpr FileIdentifier file_identifier = (B << 24) | FileIdentifierFor<T>::value;
};

template <class T, uint32_t B>
struct ComposedIdentifier<T, B, false> {};

template <class T, uint32_t B, bool = HasFileIdentifier<T>::value>
struct ComposedIdentifierExternal;
template <class T, uint32_t B>
struct ComposedIdentifierExternal<T, B, false> {};
template <class T, uint32_t B>
struct ComposedIdentifierExternal<T, B, true> {
	static constexpr FileIdentifier value = ComposedIdentifier<T, B>::file_identifier;
};

template <class T>
constexpr FileIdentifier scalar_file_identifier() {
	if constexpr (std::is_integral_v<T>) {
		if constexpr (std::is_signed_v<T>) {
			switch (sizeof(T)) {
			case 1:
				return 9;
			case 2:
				return 7;
			case 4:
				return 1;
			case 8:
				return 3;
			}
		} else {
			switch (sizeof(T)) {
			case 1:
				return 10;
			case 2:
				return 8;
			case 4:
				return 2;
			case 8:
				return 4;
			}
		}
	} else if constexpr (std::is_floating_point_v<T>) {
		switch (sizeof(T)) {
		case 4:
			return 7266212;
		case 8:
			return 9348150;
		}
	}
}

template <>
struct FileIdentifierFor<int> {
	constexpr static FileIdentifier value = 1;
};

template <>
struct FileIdentifierFor<unsigned> {
	constexpr static FileIdentifier value = 2;
};

template <>
struct FileIdentifierFor<long> {
	constexpr static FileIdentifier value = 3;
};

template <>
struct FileIdentifierFor<unsigned long> {
	constexpr static FileIdentifier value = 4;
};

template <>
struct FileIdentifierFor<short> {
	constexpr static FileIdentifier value = 7;
};

template <>
struct FileIdentifierFor<unsigned short> {
	constexpr static FileIdentifier value = 8;
};

template <>
struct FileIdentifierFor<signed char> {
	constexpr static FileIdentifier value = 9;
};

template <>
struct FileIdentifierFor<unsigned char> {
	constexpr static FileIdentifier value = 10;
};

template <>
struct FileIdentifierFor<float> {
	constexpr static FileIdentifier value = 7266212;
};

template <>
struct FileIdentifierFor<double> {
	constexpr static FileIdentifier value = 9348150;
};

static_assert(FileIdentifierFor<int>::value == scalar_file_identifier<int>());
static_assert(FileIdentifierFor<unsigned>::value == scalar_file_identifier<unsigned>());
static_assert(FileIdentifierFor<long>::value == scalar_file_identifier<long>());
static_assert(FileIdentifierFor<unsigned long>::value == scalar_file_identifier<unsigned long>());
static_assert(FileIdentifierFor<short>::value == scalar_file_identifier<short>());
static_assert(FileIdentifierFor<unsigned short>::value == scalar_file_identifier<unsigned short>());
static_assert(FileIdentifierFor<signed char>::value == scalar_file_identifier<signed char>());
static_assert(FileIdentifierFor<unsigned char>::value == scalar_file_identifier<unsigned char>());
static_assert(FileIdentifierFor<float>::value == scalar_file_identifier<float>());
static_assert(FileIdentifierFor<double>::value == scalar_file_identifier<double>());
