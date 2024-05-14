// © Alfredo A. Correa 2019-2024

#define BOOST_TEST_MODULE "C++ Unit Tests for Multi BLAS/cuBLAS iamax"
#define BOOST_TEST_DYN_LINK

#include<boost/test/unit_test.hpp>

#include "../../blas/iamax.hpp"

#include "../../../array.hpp"
#include "../../../adaptors/cuda.hpp"
#include "../../../adaptors/blas/cuda.hpp"

#include<complex>

using std::cout;
namespace multi = boost::multi;
namespace blas = multi::blas;

using complex = std::complex<double>; constexpr complex I{0.0, 1.0};

BOOST_AUTO_TEST_CASE(multi_adaptors_blas_iamax){
	multi::array<complex, 2> const A = {
		{1.0 + 2.*I,  2.0        ,  3.0       ,  4.0},
		{5.0       ,  6.0 + 3.0*I,  7.0       ,  8.0},
		{9.0       , 10.0        , 11.0 + 4.*I, 12.0},
	};

	using blas::iamax;

	auto chess = [](auto const& a, auto const& b){
		using std::abs; 
		return abs(real(a))+abs(imag(a)) < abs(real(b))+abs(imag(b));
	};

	BOOST_REQUIRE(iamax(A[1])==std::max_element(begin(A[1]), end(A[1]), chess)-begin(A[1]));
	BOOST_REQUIRE(A[1][iamax(A[1])]==*std::max_element(begin(A[1]), end(A[1]), chess));
}

BOOST_AUTO_TEST_CASE(multi_adaptors_blas_iamax_cuda){
	multi::cuda::array<complex, 2> const A = {
		{1.0 + 2.0*I,  2.0        ,  3.0        ,  4.0},
		{5.0        ,  6.0 + 3.0*I,  7.0        ,  8.0},
		{9.0        , 10.0        , 11.0 + 4.0*I, 12.0}
	};
	using blas::iamax;
	BOOST_REQUIRE(iamax(A[1]) == 1);
}
