/*
 * Copyright 1993-2010 NVIDIA Corporation.  All rights reserved.
 *
 * NVIDIA Corporation and its licensors retain all intellectual property and 
 * proprietary rights in and to this software and related documentation. 
 * Any use, reproduction, disclosure, or distribution of this software 
 * and related documentation without an express license agreement from
 * NVIDIA Corporation is strictly prohibited.
 *
 * Please refer to the applicable NVIDIA end user license agreement (EULA) 
 * associated with this source code for terms and conditions that govern 
 * your use of this NVIDIA software.
 * 
 */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#endif

#include <opencv2/gpu/gpu.hpp>
#include <opencv2/gpu/devmem2d.hpp>
#include "cv/common.hpp" //copied from opencv
#include "../OtherUtil.hpp"
#include <math_constants.h>
#include "pcl/limits.hpp"
#include "pcl/device.hpp"
#include <vector>

namespace btl
{
namespace device
{

__global__ void kernelTestFloat3(const cv::gpu::DevMem2D_<float3> cvgmIn_, cv::gpu::DevMem2D_<float3> cvgmOut_)
{
	const int nX = blockDim.x * blockIdx.x + threadIdx.x;
    const int nY = blockDim.y * blockIdx.y + threadIdx.y;

	const float3& in = cvgmIn_.ptr(nY)[nX];
	float3& out  = cvgmOut_.ptr(nY)[nX];
	out.x = out.y = out.z = (in.x + in.y + in.z)/3;
}
void cudaTestFloat3( const cv::gpu::GpuMat& cvgmIn_, cv::gpu::GpuMat* pcvgmOut_ )
{
	pcvgmOut_->create(cvgmIn_.size(),CV_32FC3);
	//define grid and block
	dim3 block(32, 8);
    dim3 grid(cv::gpu::divUp(cvgmIn_.cols, block.x), cv::gpu::divUp(cvgmIn_.rows, block.y));
	//run kernel
	kernelTestFloat3<<<grid,block>>>( cvgmIn_,*pcvgmOut_ );
	cudaSafeCall ( cudaGetLastError () );
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////
//depth to disparity
__global__ void kernelInverse(const cv::gpu::DevMem2Df cvgmIn_, cv::gpu::DevMem2Df cvgmOut_){
    const int nX = blockDim.x * blockIdx.x + threadIdx.x;
    const int nY = blockDim.y * blockIdx.y + threadIdx.y;

	if(fabsf(cvgmIn_.ptr(nY)[nX]) > 0.f )
		cvgmOut_.ptr(nY)[nX] = 1.f/cvgmIn_.ptr(nY)[nX];
	else
		cvgmOut_.ptr(nY)[nX] = pcl::device::numeric_limits<float>::quiet_NaN();
}//kernelInverse

void cudaDepth2Disparity( const cv::gpu::GpuMat& cvgmDepth_, cv::gpu::GpuMat* pcvgmDisparity_ ){
	//not necessary as pcvgmDisparity has been allocated in VideoSourceKinect()
	//pcvgmDisparity_->create(cvgmDepth_.size(),CV_32F);
	//define grid and block
	dim3 block(32, 8);
    dim3 grid(cv::gpu::divUp(cvgmDepth_.cols, block.x), cv::gpu::divUp(cvgmDepth_.rows, block.y));
	//run kernel
	kernelInverse<<<grid,block>>>( cvgmDepth_,*pcvgmDisparity_ );
	cudaSafeCall ( cudaGetLastError () );
}//cudaDepth2Disparity

void cudaDisparity2Depth( const cv::gpu::GpuMat& cvgmDisparity_, cv::gpu::GpuMat* pcvgmDepth_ ){
	pcvgmDepth_->create(cvgmDisparity_.size(),CV_32F);
	//define grid and block
	dim3 block(32, 8);
    dim3 grid(cv::gpu::divUp(cvgmDisparity_.cols, block.x), cv::gpu::divUp(cvgmDisparity_.rows, block.y));
	//run kernel
	kernelInverse<<<grid,block>>>( cvgmDisparity_,*pcvgmDepth_ );
	cudaSafeCall ( cudaGetLastError () );
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////
//global constant used by kernelUnprojectIR() and cudaUnProjectIR()
__constant__ float _aIRCameraParameter[4];// 1/f_x, 1/f_y, u, v for IR camera; constant memory declaration

__global__ void kernelUnprojectIRCVmmCVm(const cv::gpu::DevMem2Df cvgmDepth_,
	cv::gpu::DevMem2D_<float3> cvgmIRWorld_) {
    const int nX = blockDim.x * blockIdx.x + threadIdx.x;
    const int nY = blockDim.y * blockIdx.y + threadIdx.y;

	if (nX < cvgmIRWorld_.cols && nY < cvgmIRWorld_.rows) {
		const float& fDepth = cvgmDepth_.ptr(nY)[nX];
		float3& temp = cvgmIRWorld_.ptr(nY)[nX];
		
		if(400.f < fDepth && fDepth < 4000.f ){ //truncate, fDepth is captured from openni and always > 0
			temp.z = fDepth /1000.f;//convert to meter z 5 million meter is added according to experience. as the OpenNI
			//coordinate system is defined w.r.t. the camera plane which is 0.5 centimeters in front of the camera center
			temp.x = (nX - _aIRCameraParameter[2]) * _aIRCameraParameter[0] * temp.z;
			temp.y = (nY - _aIRCameraParameter[3]) * _aIRCameraParameter[1] * temp.z;
		}//if within 0.4m - 4m
		else{
			temp.x = temp.y = temp.z = pcl::device::numeric_limits<float>::quiet_NaN();
		}//else
	}//if inside image
	return;
}//kernelUnprojectIRCVCV

void cudaUnprojectIRCVCV(const cv::gpu::GpuMat& cvgmDepth_ ,
const float& fFxIR_, const float& fFyIR_, const float& uIR_, const float& vIR_, 
cv::gpu::GpuMat* pcvgmIRWorld_ )
{
	//constant definition
	size_t sN = sizeof(float) * 4;
	float* const pIRCameraParameters = (float*) malloc( sN );
	pIRCameraParameters[0] = 1.f/fFxIR_;
	pIRCameraParameters[1] = 1.f/fFyIR_;
	pIRCameraParameters[2] = uIR_;
	pIRCameraParameters[3] = vIR_;
	cudaSafeCall( cudaMemcpyToSymbol(_aIRCameraParameter, pIRCameraParameters, sN) );
	//define grid and block
	dim3 block(32, 8);
    dim3 grid(cv::gpu::divUp(cvgmDepth_.cols, block.x), cv::gpu::divUp(cvgmDepth_.rows, block.y));
	//run kernel
    kernelUnprojectIRCVmmCVm<<<grid,block>>>( cvgmDepth_,*pcvgmIRWorld_ );
	cudaSafeCall ( cudaGetLastError () );
	//release temporary pointers
	free(pIRCameraParameters);
	return;
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////
//global constant used by kernelUnprojectIR() and cudaTransformIR2RGB()
__constant__ float _aR[9];
__constant__ float _aRT[3];
__global__ void kernelTransformIR2RGBCVmCVm(const cv::gpu::DevMem2D_<float3> cvgmIRWorld_, cv::gpu::DevMem2D_<float3> cvgmRGBWorld_){
    const int nX = blockDim.x * blockIdx.x + threadIdx.x;
    const int nY = blockDim.y * blockIdx.y + threadIdx.y;

	if (nX >= cvgmRGBWorld_.cols || nY >= cvgmRGBWorld_.rows) return;

	float3& rgbWorld = cvgmRGBWorld_.ptr(nY)[nX];
	const float3& irWorld  = cvgmIRWorld_ .ptr(nY)[nX];
	if( 0.4f < irWorld.z && irWorld.z < 4.f ) {
		//_aR[0] [1] [2] //row major
		//   [3] [4] [5]
		//   [6] [7] [8]
		//_aT[0]
		//   [1]
		//   [2]
		//  pRGB_ = _aR * ( pIR_ - _aT )
		//  	  = _aR * pIR_ - _aR * _aT
		//  	  = _aR * pIR_ - _aRT
		rgbWorld.x = _aR[0] * irWorld.x + _aR[1] * irWorld.y + _aR[2] * irWorld.z - _aRT[0];
		rgbWorld.y = _aR[3] * irWorld.x + _aR[4] * irWorld.y + _aR[5] * irWorld.z - _aRT[1];
		rgbWorld.z = _aR[6] * irWorld.x + _aR[7] * irWorld.y + _aR[8] * irWorld.z - _aRT[2];
	}//if irWorld.z within 0.4m-4m
	else{
		rgbWorld.x = rgbWorld.y = rgbWorld.z = pcl::device::numeric_limits<float>::quiet_NaN();
	}//set NaN
	return;
}//kernelTransformIR2RGB
void cudaTransformIR2RGBCVCV(const cv::gpu::GpuMat& cvgmIRWorld_, const float* aR_, const float* aRT_, cv::gpu::GpuMat* pcvgmRGBWorld_){
	cudaSafeCall( cudaMemcpyToSymbol(_aR,  aR_,  9*sizeof(float)) );
	cudaSafeCall( cudaMemcpyToSymbol(_aRT, aRT_, 3*sizeof(float)) );
	//define grid and block
	dim3 block(32, 8);
    dim3 grid(cv::gpu::divUp(pcvgmRGBWorld_->cols, block.x), cv::gpu::divUp(pcvgmRGBWorld_->rows, block.y));
	//run kernel
    kernelTransformIR2RGBCVmCVm<<<grid,block>>>( cvgmIRWorld_,*pcvgmRGBWorld_ );
	cudaSafeCall ( cudaGetLastError () );
	return;
}//cudaTransformIR2RGB
//////////////////////////////////////////////////////////////////////////////////////////////////////////////
//global constant used by kernelProjectRGB() and cudaProjectRGB()
__constant__ float _aRGBCameraParameter[4]; //fFxRGB_,fFyRGB_,uRGB_,vRGB_
__global__ void kernelProjectRGBCVmCVm(const cv::gpu::DevMem2D_<float3> cvgmRGBWorld_, cv::gpu::DevMem2Df cvgmAligned_){
    const int nX = blockDim.x * blockIdx.x + threadIdx.x;
    const int nY = blockDim.y * blockIdx.y + threadIdx.y;
	// cvgmAligned_ must be preset to zero;
	if (nX >= cvgmRGBWorld_.cols || nY >= cvgmRGBWorld_.rows) return;
	const float3& rgbWorld = cvgmRGBWorld_.ptr(nY)[nX];
	if( 0.4f < rgbWorld.z  &&  rgbWorld.z  < 4.f ){
		// get 2D image projection in RGB image of the XYZ in the world
		int nXAligned = __float2int_rn( _aRGBCameraParameter[0] * rgbWorld.x / rgbWorld.z + _aRGBCameraParameter[2] );
		int nYAligned = __float2int_rn( _aRGBCameraParameter[1] * rgbWorld.y / rgbWorld.z + _aRGBCameraParameter[3] );
		if ( nXAligned >= 0 && nXAligned < cvgmRGBWorld_.cols && nYAligned >= 0 && nYAligned < cvgmRGBWorld_.rows )	{
			float fPt = cvgmAligned_.ptr(nYAligned)[nXAligned];
			if(isnan<float>(fPt)){
				cvgmAligned_.ptr(nYAligned)[nXAligned] = rgbWorld.z;
			}//if havent been asigned
			else{
				fPt = (fPt+ rgbWorld.z)/2.f;
			}//if it does use the average 
		}//if inside rgb
	}//if within 0.4m-4m
	//else is not required
	//the cvgmAligned_ is preset to NaN
	return;
}//kernelProjectRGB
void cudaProjectRGBCVCV(const cv::gpu::GpuMat& cvgmRGBWorld_, 
const float& fFxRGB_, const float& fFyRGB_, const float& uRGB_, const float& vRGB_, 
cv::gpu::GpuMat* pcvgmAligned_ ){
	//constant definition
	size_t sN = sizeof(float) * 4;
	float* const pRGBCameraParameters = (float*) malloc( sN );
	pRGBCameraParameters[0] = fFxRGB_;
	pRGBCameraParameters[1] = fFyRGB_;
	pRGBCameraParameters[2] = uRGB_;
	pRGBCameraParameters[3] = vRGB_;
	cudaSafeCall( cudaMemcpyToSymbol(_aRGBCameraParameter, pRGBCameraParameters, sN) );
	//define grid and block
	dim3 block(32, 8);
    dim3 grid(cv::gpu::divUp(cvgmRGBWorld_.cols, block.x), cv::gpu::divUp(cvgmRGBWorld_.rows, block.y));
	//run kernel
    kernelProjectRGBCVmCVm<<<grid,block>>>( cvgmRGBWorld_,*pcvgmAligned_ );
	cudaSafeCall ( cudaGetLastError () );
	//release temporary pointers
	free(pRGBCameraParameters);
	return;
}//cudaProjectRGBCVCV()
//////////////////////////////////////////////////////////////////////////////////////////////////////////////
//const float sigma_color = 30;     //in mm
//const float sigma_space = 4;     // in pixels
__constant__ float _aSigma2InvHalf[2]; //sigma_space2_inv_half,sigma_color2_inv_half

__global__ void kernelBilateral (const cv::gpu::DevMem2Df src, cv::gpu::DevMem2Df dst )
{
    int x = threadIdx.x + blockIdx.x * blockDim.x;
    int y = threadIdx.y + blockIdx.y * blockDim.y;

    if (x >= src.cols || y >= src.rows)  return;

    const int R = 2;//static_cast<int>(sigma_space * 1.5);
    const int D = R * 2 + 1;

    float fValueCentre = src.ptr (y)[x];
	//if fValueCentre is NaN
	if(fValueCentre!=fValueCentre) return; 

    int tx = min (x - D/2 + D, src.cols - 1);
    int ty = min (y - D/2 + D, src.rows - 1);

    float sum1 = 0;
    float sum2 = 0;

    for (int cy = max (y - D/2, 0); cy < ty; ++cy)
    for (int cx = max (x - D/2, 0); cx < tx; ++cx){
        float  fValueNeighbour = src.ptr (cy)[cx];
		//if fValueNeighbour is NaN
		if(fValueNeighbour!=fValueNeighbour) continue; 
        float space2 = (x - cx) * (x - cx) + (y - cy) * (y - cy);
        float color2 = (fValueCentre - fValueNeighbour) * (fValueCentre - fValueNeighbour);
        float weight = __expf (-(space2 * _aSigma2InvHalf[0] + color2 * _aSigma2InvHalf[1]) );

        sum1 += fValueNeighbour * weight;
        sum2 += weight;
    }//for for each pixel in neigbbourhood

    dst.ptr (y)[x] = sum1/sum2;
	return;
}//kernelBilateral

void cudaBilateralFiltering(const cv::gpu::GpuMat& cvgmSrc_, const float& fSigmaSpace_, const float& fSigmaColor_, cv::gpu::GpuMat* pcvgmDst_ )
{
	//constant definition
	size_t sN = sizeof(float) * 2;
	float* const pSigma = (float*) malloc( sN );
	pSigma[0] = 0.5f / (fSigmaSpace_ * fSigmaSpace_);
	pSigma[1] = 0.5f / (fSigmaColor_ * fSigmaColor_);
	cudaSafeCall( cudaMemcpyToSymbol(_aSigma2InvHalf, pSigma, sN) );
	//define grid and block
	dim3 block(32, 8);
    dim3 grid(cv::gpu::divUp(cvgmSrc_.cols, block.x), cv::gpu::divUp(cvgmSrc_.rows, block.y));
	//run kernel
    kernelBilateral<<<grid,block>>>( cvgmSrc_,*pcvgmDst_ );
	cudaSafeCall ( cudaGetLastError () );
	//release temporary pointers
	free(pSigma);
	return;
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////
__global__ void kernelPyrDown (const cv::gpu::DevMem2Df cvgmSrc_, cv::gpu::DevMem2Df cvgmDst_, float fSigmaColor_ )
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;

    if (x >= cvgmDst_.cols || y >= cvgmDst_.rows) return;

    const int D = 5;

    float center = cvgmSrc_.ptr (2 * y)[2 * x];
	if( center!=center ){
		cvgmDst_.ptr (y)[x] = pcl::device::numeric_limits<float>::quiet_NaN();
		return;
	}//if center is NaN
    int tx = min (2 * x - D / 2 + D, cvgmSrc_.cols - 1);
    int ty = min (2 * y - D / 2 + D, cvgmSrc_.rows - 1);
    int cy = max (0, 2 * y - D / 2);

    float sum = 0;
    int count = 0;

    for (; cy < ty; ++cy)
    for (int cx = max (0, 2 * x - D / 2); cx < tx; ++cx) {
        float val = cvgmSrc_.ptr (cy)[cx];
        if (fabsf (val - center) < 3 * fSigmaColor_){//
			sum += val;
			++count;
        } //if within 3*fSigmaColor_
    }//for each pixel in the neighbourhood
    cvgmDst_.ptr (y)[x] = sum / count;
}//kernelPyrDown()
void cudaPyrDown (const cv::gpu::GpuMat& cvgmSrc_, const float& fSigmaColor_, cv::gpu::GpuMat* pcvgmDst_)
{
	dim3 block (32, 8);
	dim3 grid (cv::gpu::divUp (pcvgmDst_->cols, block.x), cv::gpu::divUp (pcvgmDst_->rows, block.y));
	kernelPyrDown<<<grid, block>>>(cvgmSrc_, *pcvgmDst_, fSigmaColor_);
	cudaSafeCall ( cudaGetLastError () );
};
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////
__global__ void kernelUnprojectRGBCVmBOTHm (const cv::gpu::DevMem2Df cvgmDepths_, const unsigned short uScale_, cv::gpu::DevMem2D_<float3> cvgmPts_,
	btl::utility::tp_coordinate_convention eConvention_ )
{
    const int nX = blockDim.x * blockIdx.x + threadIdx.x;
    const int nY = blockDim.y * blockIdx.y + threadIdx.y;

    if (nX >= cvgmPts_.cols || nY >= cvgmPts_.rows)  return;

	float3& pt = cvgmPts_.ptr(nY)[nX];
	const float fDepth = cvgmDepths_.ptr(nY)[nX];

	if( 0.4f < fDepth && fDepth < 4.f ){
		pt.z = fDepth;
		pt.x = ( nX*uScale_  - _aRGBCameraParameter[2] ) * _aRGBCameraParameter[0] * pt.z; //_aRGBCameraParameter[0] is 1.f/fFxRGB_
		pt.y = ( nY*uScale_  - _aRGBCameraParameter[3] ) * _aRGBCameraParameter[1] * pt.z; 
		//convert from opencv convention to opengl convention
		if( btl::utility::tp_coordinate_convention::BTL_GL == eConvention_ ){
			pt.y = -pt.y;
			pt.z = -pt.z;
		}
	}
	else {
		pt.x = pt.y = pt.z = pcl::device::numeric_limits<float>::quiet_NaN();
	}
}
void cudaUnprojectRGBCVBOTH ( const cv::gpu::GpuMat& cvgmDepths_, 
	const float& fFxRGB_,const float& fFyRGB_,const float& uRGB_, const float& vRGB_, unsigned int uLevel_, 
	cv::gpu::GpuMat* pcvgmPts_, btl::utility::tp_coordinate_convention eConvention_ /*= btl::utility::tp_coordinate_convention::BTL_GL*/ )
{
	unsigned short uScale = 1<< uLevel_;
	pcvgmPts_->setTo(0);
	//constant definition
	size_t sN = sizeof(float) * 4;
	float* const pRGBCameraParameters = (float*) malloc( sN );
	pRGBCameraParameters[0] = 1.f/fFxRGB_;
	pRGBCameraParameters[1] = 1.f/fFyRGB_;
	pRGBCameraParameters[2] = uRGB_;
	pRGBCameraParameters[3] = vRGB_;
	cudaSafeCall( cudaMemcpyToSymbol(_aRGBCameraParameter, pRGBCameraParameters, sN) );
	
	dim3 block (32, 8);
	dim3 grid (cv::gpu::divUp (pcvgmPts_->cols, block.x), cv::gpu::divUp (pcvgmPts_->rows, block.y));
	kernelUnprojectRGBCVmBOTHm<<<grid, block>>>(cvgmDepths_, uScale, *pcvgmPts_, eConvention_ );
	cudaSafeCall ( cudaGetLastError () );
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////
__global__ void kernelFastNormalEstimation (const cv::gpu::DevMem2D_<float3> cvgmPts_, cv::gpu::DevMem2D_<float3> cvgmNls_ )
{
    const int nX = blockDim.x * blockIdx.x + threadIdx.x;
    const int nY = blockDim.y * blockIdx.y + threadIdx.y;

    if (nX >= cvgmPts_.cols-1 || nY >= cvgmPts_.rows-1) return;

	const float3& pt = cvgmPts_.ptr(nY)[nX];
	const float3& pt1= cvgmPts_.ptr(nY)[nX+1]; //right 
	const float3& pt2= cvgmPts_.ptr(nY+1)[nX]; //down

	float3& fN = cvgmNls_.ptr(nY)[nX];

	if(pt.z!=pt.z||pt1.z!=pt1.z||pt2.z!=pt2.z){
		fN.x = pcl::device::numeric_limits<float>::quiet_NaN();
		fN.y = pcl::device::numeric_limits<float>::quiet_NaN();
		fN.z = pcl::device::numeric_limits<float>::quiet_NaN();
		return;
	}//if input or its neighour is NaN,
	float3 v1;
	v1.x = pt1.x-pt.x;
	v1.y = pt1.y-pt.y;
	v1.z = pt1.z-pt.z;
	float3 v2;
	v2.x = pt2.x-pt.x;
	v2.y = pt2.y-pt.y;
	v2.z = pt2.z-pt.z;
	//n = v1 x v2 cross product
	float3 n;
	n.x = v1.y*v2.z - v1.z*v2.y;
	n.y = v1.z*v2.x - v1.x*v2.z;
	n.z = v1.x*v2.y - v1.y*v2.x;
	//normalization
	float norm = sqrtf(n.x*n.x + n.y*n.y + n.z*n.z);

	if( norm < 1.0e-10 ) {
		fN.x = pcl::device::numeric_limits<float>::quiet_NaN();
		fN.y = pcl::device::numeric_limits<float>::quiet_NaN();
		fN.z = pcl::device::numeric_limits<float>::quiet_NaN();
		return;
	}//set as NaN,
	n.x /= norm;
	n.y /= norm;
	n.z /= norm;

	if( -n.x*pt.x - n.y*pt.y - n.z*pt.z <0 ){ //this gives (0-pt).dot( n ); 
		fN.x = -n.x;
		fN.y = -n.y;
		fN.z = -n.z;
	}//if facing away from the camera
	else{
		fN.x = n.x;
		fN.y = n.y;
		fN.z = n.z;
	}//else
	return;
}

void cudaFastNormalEstimation(const cv::gpu::GpuMat& cvgmPts_, cv::gpu::GpuMat* pcvgmNls_ )
{
	pcvgmNls_->setTo(0);
	dim3 block (32, 8);
	dim3 grid (cv::gpu::divUp (cvgmPts_.cols, block.x), cv::gpu::divUp (cvgmPts_.rows, block.y));
	kernelFastNormalEstimation<<<grid, block>>>(cvgmPts_, *pcvgmNls_ );
	cudaSafeCall ( cudaGetLastError () );
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//GL is always measured in meters
__global__ void kernelNormalSetRotationAxisCVmGL (const cv::gpu::DevMem2D_<float3> cvgmNlsCV_, cv::gpu::DevMem2D_<float3> cvgmAAs_ )
{
    const int nX = blockDim.x * blockIdx.x + threadIdx.x;
    const int nY = blockDim.y * blockIdx.y + threadIdx.y;
    if (nX >= cvgmNlsCV_.cols || nY >= cvgmNlsCV_.rows ) return;
	const float3& Nl = cvgmNlsCV_.ptr(nY)[nX];
	float3& fRA = cvgmAAs_.ptr(nY)[nX];
	if(isnan<float>(Nl.x)||isnan<float>(Nl.y)) {
		fRA.x=fRA.y=fRA.z=pcl::device::numeric_limits<float>::quiet_NaN();
		return;
	}//if is NaN
	//Assuming both vectors v1, v2 are of equal magnitude, 
	//a unique rotation R about the origin exists satisfying R.z-axis = Nl.
	//It is most easily expressed in axis-angle representation.
	//First, normalise the two source vectors, then compute w = z-axis � Nl (z-axis 0,0,1) Nl (x,-y,-z)
	//Normalise again for the axis: w' = w / |w|
	//Take the arcsine of the magnitude for the angle: 
	//q = asin(|w|)

	//float3 n;
	//n.x = Nl.y; //because of cv-convention
	//n.y = Nl.x;
	//n.z =  0;
	//normalization
	float norm = sqrtf(Nl.x*Nl.x + Nl.y*Nl.y );
	if(norm >0.f){
		fRA.x = Nl.y/norm;
		fRA.y = Nl.x/norm;
		fRA.z = asinf(norm)*180.f/CUDART_PI_F;//convert to degree
	}else{
		fRA.x=fRA.y=fRA.z=pcl::device::numeric_limits<float>::quiet_NaN();
	}

	return;
}//kernelNormalCVSetRotationAxisGL()

void cudaNormalSetRotationAxisCVGL(const cv::gpu::GpuMat& cvgmNlsCV_, cv::gpu::GpuMat* pcvgmAAs_ )
{
	pcvgmAAs_->setTo(0);
	dim3 block (32, 8);
	dim3 grid (cv::gpu::divUp (cvgmNlsCV_.cols, block.x), cv::gpu::divUp (cvgmNlsCV_.rows, block.y));
	kernelNormalSetRotationAxisCVmGL<<<grid, block>>>(cvgmNlsCV_, *pcvgmAAs_ );
	cudaSafeCall ( cudaGetLastError () );
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////
__constant__ ushort _aNormalHistorgarmParams[3];
__global__ void kernelNormalHistogramKernelCV (const cv::gpu::DevMem2D_<float3> cvgmNlsCV_, const float fNormalBinSize_, cv::gpu::DevMem2D_<short> cvgmBinIdx_ ){

	int nX = threadIdx.x + blockIdx.x * blockDim.x;
    int nY = threadIdx.y + blockIdx.y * blockDim.y;
	if (nX >= cvgmNlsCV_.cols || nY >= cvgmNlsCV_.rows)  return;
	const float3& nl = cvgmNlsCV_.ptr (nY)[nX];
	if( isnan<float>(nl.x)||isnan<float>(nl.y)||isnan<float>(nl.z) ) return;

	ushort usX,usY,usZ;
	usX = __float2int_rd( nl.x / fNormalBinSize_ )+_aNormalHistorgarmParams[0];//0:usSamplesElevationZ_
	usY = __float2int_rd( nl.y / fNormalBinSize_ )+_aNormalHistorgarmParams[0];
	usZ = __float2int_rd(-nl.z / fNormalBinSize_ ); //because of cv-convention
	cvgmBinIdx_.ptr(nY)[nX]= usZ*_aNormalHistorgarmParams[2]+ usY*_aNormalHistorgarmParams[1]+ usX;//2:usLevel 1:usWidth
}//kernelNormalHistogramKernelCV()
void cudaNormalHistogramCV(const cv::gpu::GpuMat& cvgmNlsCV_, const unsigned short usSamplesAzimuth_, const unsigned short usSamplesElevationZ_, 
	const unsigned short usWidth_,const unsigned short usLevel_,  const float fNormalBinSize_, cv::gpu::GpuMat* pcvgmBinIdx_){
	//constant definition
	size_t sN = sizeof(ushort) * 3;
	ushort* const pNormal = (ushort*) malloc( sN );
	pNormal[0] = usSamplesElevationZ_;
	pNormal[1] = usWidth_;
	pNormal[2] = usLevel_;
	cudaSafeCall( cudaMemcpyToSymbol(_aNormalHistorgarmParams, pNormal, sN) );
	//define grid and block
	dim3 block(32, 8);
    dim3 grid(cv::gpu::divUp(cvgmNlsCV_.cols, block.x), cv::gpu::divUp(cvgmNlsCV_.rows, block.y));
	kernelNormalHistogramKernelCV<<<grid,block>>>(cvgmNlsCV_,fNormalBinSize_,*pcvgmBinIdx_);
	cudaSafeCall ( cudaGetLastError () );
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////
__constant__ float _aParam[2];//0:_fThreshold;1:_fSize
//tried pcl style but havent figure it out
//__global__ void kernelThresholdVolumeCVGL(const cv::gpu::DevMem2D_<short2> cvgmYZxXVolume_,cv::gpu::DevMem2D_<float3> cvgmYZxXVolCenter_){
//	int nY = threadIdx.x + blockIdx.x * blockDim.x; //as the volume is y*z cols and x rows
//    int nX = threadIdx.y + blockIdx.y * blockDim.y; 
//	if (nX >= cvgmYZxXVolume_.rows && nY >= cvgmYZxXVolume_.rows) return; //both nX and nX and bounded by cols as the structure is a cubic
//	int nElemStep = sizeof(short2);
//	int nElemStepC = sizeof(float3);
//    const short2* pZ = cvgmYZxXVolume_.ptr(nX)+nY*cvgmYZxXVolume_.rows*nElemStep;
//	float3 *pZCenter = cvgmYZxXVolCenter_.ptr(nX)+nY*cvgmYZxXVolCenter_.rows*nElemStepC;
//	int nHalfCols = cvgmYZxXVolume_.rows/2;
//	float fHalfVoxelSize = _aParam[1]/2.f;
//	for (int nZ = 0; nZ < cvgmYZxXVolume_.rows; ++nZ, pZ += nElemStep, pZCenter += nElemStepC) {
//		float fTSDF = pcl::device::unpack_tsdf(*pZ);
//		if(fabsf(fTSDF)<_aParam[0]){
//			pZCenter->x = (nX - nHalfCols)*_aParam[1] - fHalfVoxelSize;
//			pZCenter->y =-(nY - nHalfCols)*_aParam[1] - fHalfVoxelSize;// - convert from cv to GL
//			pZCenter->z =-(nZ - nHalfCols)*_aParam[1] - fHalfVoxelSize;// - convert from cv to GL
//		}//within threshold
//		else{
//			pZCenter->x = pZCenter->y = pZCenter->z = pcl::device::numeric_limits<float>::quiet_NaN();
//		}
//
//	}//for each Z
//	return;
//}//kernelThresholdVolume()
__global__ void kernelThresholdVolume2by2CVGL(const cv::gpu::DevMem2D_<short2> cvgmYZxXVolume_,cv::gpu::DevMem2D_<float3> cvgmYZxXVolCenter_){
	int nX = threadIdx.x + blockIdx.x * blockDim.x; // for each y*z z0,z1,...
    int nY = threadIdx.y + blockIdx.y * blockDim.y; 
	if (nX >= cvgmYZxXVolume_.cols && nY >= cvgmYZxXVolume_.rows) return; //both nX and nX and bounded by cols as the structure is a cubic

    const short2& sValue = cvgmYZxXVolume_.ptr(nY)[nX];
	float3& fCenter = cvgmYZxXVolCenter_.ptr(nY)[nX];
	
	int nHalfCols = cvgmYZxXVolume_.rows/2;
	float fHalfVoxelSize = _aParam[1]/2.f; //_aParam[1] is voxel size

	int nGridX = nY;
	int nGridY = nX/cvgmYZxXVolume_.rows;
	int nGridZ = nX%cvgmYZxXVolume_.rows;
	float fTSDF = pcl::device::unpack_tsdf(sValue);
	if(fabsf(fTSDF)<_aParam[0]){
		fCenter.x = (nGridX - nHalfCols)*_aParam[1] - fHalfVoxelSize;
		fCenter.y =-(nGridY - nHalfCols)*_aParam[1] - fHalfVoxelSize;// - convert from cv to GL
		fCenter.z =-(nGridZ - nHalfCols)*_aParam[1] - fHalfVoxelSize;// - convert from cv to GL
	}//within threshold
	else{
		fCenter.x = fCenter.y = fCenter.z = pcl::device::numeric_limits<float>::quiet_NaN();
	}
	return;
}//kernelThresholdVolume()
void thresholdVolumeCVGL(const cv::gpu::GpuMat& cvgmYZxXVolume_, const float fThreshold_, const float fVoxelSize_, const cv::gpu::GpuMat* pcvgmYZxXVolCenter_){
	size_t sN = sizeof(float)*2;
	float* const pParam = (float*) malloc( sN );
	pParam[0] = fThreshold_;
	pParam[1] = fVoxelSize_;
	cudaSafeCall( cudaMemcpyToSymbol(_aParam, pParam, sN) );
	dim3 block(32, 8);
    dim3 grid(cv::gpu::divUp(cvgmYZxXVolume_.cols, block.x), cv::gpu::divUp(cvgmYZxXVolume_.rows, block.y));
	//kernelThresholdVolumeCVGL<<<grid,block>>>(cvgmYZxXVolume_,*pcvgmYZxXVolCenter_);
	kernelThresholdVolume2by2CVGL<<<grid,block>>>(cvgmYZxXVolume_,*pcvgmYZxXVolCenter_);
	cudaSafeCall ( cudaGetLastError () );
}//thresholdVolume()
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////
__global__ void kernelScaleDepthCVmCVm (cv::gpu::DevMem2Df cvgmDepth_, const pcl::device::Intr sCameraIntrinsics_)
{
    int nX = threadIdx.x + blockIdx.x * blockDim.x;
    int nY = threadIdx.y + blockIdx.y * blockDim.y;

    if (nX >= cvgmDepth_.cols || nY >= cvgmDepth_.rows)  return;

    float& fDepth = cvgmDepth_.ptr(nY)[nX];
    float fTanX = (nX - sCameraIntrinsics_.cx) / sCameraIntrinsics_.fx;
    float fTanY = (nY - sCameraIntrinsics_.cy) / sCameraIntrinsics_.fy;
    float fSec = sqrtf (fTanX*fTanX + fTanY*fTanY + 1);
    fDepth *= fSec; //meters
}//kernelScaleDepthCVmCVm()
void scaleDepthCVmCVm(unsigned short usPyrLevel_, const float fFx_, const float fFy_, const float u_, const float v_, cv::gpu::GpuMat* pcvgmDepth_){
	pcl::device::Intr sCameraIntrinsics(fFx_,fFy_,u_,v_);
	dim3 block(32, 8);
    dim3 grid(cv::gpu::divUp(pcvgmDepth_->cols, block.x), cv::gpu::divUp(pcvgmDepth_->rows, block.y));
	kernelScaleDepthCVmCVm<<< grid,block >>>(*pcvgmDepth_,sCameraIntrinsics(usPyrLevel_));
	cudaSafeCall ( cudaGetLastError () );
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////
struct STSDF{
	enum{
        MAX_WEIGHT = 1 << 7
    };
};
__constant__ double _aRW[9]; //camera externals Rotation defined in world
__constant__ double _aTW[3]; //camera externals Translation defined in world
__constant__ double _aCW[3]; //camera center
__global__ void kernelIntegrateFrame2VolumeCVmCVm(const cv::gpu::DevMem2D_<float> cvgmDepthScaled_, pcl::device::Intr sCameraIntrinsics_, 
const float fVoxelSize_, const float fTruncDistanceM_, 
cv::gpu::DevMem2D_<short2> cvgmYZxXVolume_ ){
	int nX = threadIdx.x + blockIdx.x * blockDim.x; // for each y*z z0,z1,...
    int nY = threadIdx.y + blockIdx.y * blockDim.y; 
	if (nX >= cvgmYZxXVolume_.cols && nY >= cvgmYZxXVolume_.rows) return;
	int nHalfCols = cvgmYZxXVolume_.rows/2;
	float fHalfVoxelSize = fVoxelSize_/2.f;

	//calc grid idx
	int nGridX = nY;
	int nGridY = nX/cvgmYZxXVolume_.rows;
	int nGridZ = nX%cvgmYZxXVolume_.rows;
	//calc voxel center coordinate, 0,1|2,3 // -1.5,-0.5|0.5,1.5 //fVoxelSize = 1.0
	float3 fVoxelCenter;
	fVoxelCenter.x = (nGridX - nHalfCols)*fVoxelSize_ - fHalfVoxelSize;
	fVoxelCenter.y =-(nGridY - nHalfCols)*fVoxelSize_ - fHalfVoxelSize;// - convert from cv to GL
	fVoxelCenter.z =-(nGridZ - nHalfCols)*fVoxelSize_ - fHalfVoxelSize;// - convert from cv to GL
	//convert voxel to camera coordinate (local coordinate)
	//R*fVoxelCenter +T
	float3 fVoxelCenterLocal;
	fVoxelCenterLocal.x = _aRW[0]*fVoxelCenter.x+_aRW[3]*fVoxelCenter.y+_aRW[6]*fVoxelCenter.z+_aTW[0];
	fVoxelCenterLocal.y = _aRW[1]*fVoxelCenter.x+_aRW[4]*fVoxelCenter.y+_aRW[7]*fVoxelCenter.z+_aTW[1];
	fVoxelCenterLocal.z = _aRW[2]*fVoxelCenter.x+_aRW[5]*fVoxelCenter.y+_aRW[8]*fVoxelCenter.z+_aTW[2];
	//project voxel local to image to pick up corresponding depth
	int c = __float2int_rn((sCameraIntrinsics_.fx * fVoxelCenterLocal.x + sCameraIntrinsics_.cx * fVoxelCenterLocal.z)/fVoxelCenterLocal.z);
	int r = __float2int_rn((sCameraIntrinsics_.fy * fVoxelCenterLocal.y + sCameraIntrinsics_.cy * fVoxelCenterLocal.z)/fVoxelCenterLocal.z);
    if (c < 0 || r < 0 || c >= cvgmDepthScaled_.cols || r >= cvgmDepthScaled_.rows) return;

	//get the depthScaled
	const float& fDepth = cvgmDepthScaled_.ptr(r)[c];
	if(fDepth != fDepth) return;
	float3 Tmp; 
	Tmp.x = fVoxelCenter.x - _aCW[0];
	Tmp.y = fVoxelCenter.y - _aCW[1];
	Tmp.z = fVoxelCenter.z - _aCW[2];
	float fSignedDistance = sqrt(Tmp.x*Tmp.x + Tmp.y*Tmp.y+ Tmp.z*Tmp.z) - fDepth; //- outside + inside
	float fTrancDistInv = 1.0f / fTruncDistanceM_;
	float fTSDF;
	if(fSignedDistance > 0 ){
		 fTSDF = fmin ( 1.0f, fSignedDistance * fTrancDistInv ); 
	}
	else{
		 fTSDF = fmax (-1.0f, fSignedDistance * fTrancDistInv );
	}// truncated and normalize the Signed Distance to [-1,1]
	
	//read an unpack tsdf value and store into the volumes
	short2& sValue = cvgmYZxXVolume_.ptr(nY)[nX];
    float fTSDFPrev;
    int nWeightPrev;
	pcl::device::unpack_tsdf(sValue,fTSDFPrev,nWeightPrev);
	int nWeightNew = min(STSDF::MAX_WEIGHT,nWeightPrev+1);
	float fTSDFNew = (fTSDFPrev*nWeightPrev + fTSDF*nWeightNew)/(nWeightNew+nWeightPrev);
	pcl::device::pack_tsdf( fTSDFNew,nWeightNew,sValue);
	return;
}//kernelIntegrateFrame2VolumeCVmCVm()
void integrateFrame2VolumeCVCV(const cv::gpu::GpuMat& cvgmDepthScaled_, const unsigned short usPyrLevel_, 
const float fVoxelSize_, const float fTruncDistanceM_, 
const double* pR_, const double* pT_,  const double* pC_, 
const float fFx_, const float fFy_, const float u_, const float v_, cv::gpu::GpuMat* pcvgmYZxXVolume_){
	//pR_ is colume major 
	size_t sN1 = sizeof(double) * 9;
	cudaSafeCall( cudaMemcpyToSymbol(_aRW, pR_, sN1) );
	size_t sN2 = sizeof(double) * 3;
	cudaSafeCall( cudaMemcpyToSymbol(_aTW, pT_, sN2) );
	cudaSafeCall( cudaMemcpyToSymbol(_aCW, pC_, sN2) );
	pcl::device::Intr sCameraIntrisics(fFx_,fFy_,u_,v_);
	//define grid and block
	dim3 block(64, 4);
    dim3 grid(cv::gpu::divUp(pcvgmYZxXVolume_->cols, block.x), cv::gpu::divUp(pcvgmYZxXVolume_->rows, block.y));
	kernelIntegrateFrame2VolumeCVmCVm<<<grid,block>>>(cvgmDepthScaled_, sCameraIntrisics(usPyrLevel_), fVoxelSize_,fTruncDistanceM_, *pcvgmYZxXVolume_);
	cudaSafeCall ( cudaGetLastError () );
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////
}//device
}//btl
