#include <opencv2/opencv.hpp>
#include <opencv2/gpu/gpu.hpp>
#include <boost/shared_ptr.hpp>

#include "SemiDenseTracker.h"
#include "SemiDenseTrackerOrb.h"

#include <cuda.h>
#include <cuda_runtime.h>
#include "Helper.hpp"

#include <opencv2/gpu/device/common.hpp>

__device__ short2 operator + (const short2 s2O1_, const short2 s2O2_);
__device__ short2 operator - (const short2 s2O1_, const short2 s2O2_);
__device__ short2 operator * (const float fO1_, const short2 s2O2_);

namespace btl{ namespace device{ namespace semidense{
	unsigned int cudaCalcSaliency(const cv::gpu::GpuMat& cvgmImage_, const unsigned short usHalfSizeRound_,
									const unsigned char ucContrastThreshold_, const float& fSaliencyThreshold_, 
									cv::gpu::GpuMat* pcvgmSaliency_, cv::gpu::GpuMat* pcvgmKeyPointLocations_);
	unsigned int cudaNonMaxSupression(const cv::gpu::GpuMat& cvgmKeyPointLocation_, const unsigned int uMaxSalientPoints_, 
										const cv::gpu::GpuMat& cvgmSaliency_, short2* ps2devLocations_, float* pfdevResponse_);
	void thrustSort(short2* pnLoc_, float* pfResponse_, const unsigned int nCorners_);
	void cudaCalcAngles(const cv::gpu::GpuMat& cvgmImage_, const short2* pdevFinalKeyPointsLocations_, const unsigned int uPoints_,  const unsigned short usHalf_, 
						cv::gpu::GpuMat* pcvgmParticleAngle_);
	void loadUMax(const int* pUMax_, int nCount_);
	void cudaExtractAllDescriptorOrb(	const cv::gpu::GpuMat& cvgmImage_,
										const short2* ps2KeyPointsLocations_, const float* pfKeyPointsResponse_, 
										const unsigned int uTotalParticles_, const unsigned short usHalfPatchSize_,
										const short* psPatternX_, const short* psPatternY_,
										cv::gpu::GpuMat* pcvgmParticleResponses_, cv::gpu::GpuMat* pcvgmParticleAngle_, cv::gpu::GpuMat* pcvgmParticleDescriptor_);
	//after tracking, the matched particles are filled into the pcvgmParticleResponsesCurr_, pcvgmParticlesAgeCurr_, pcvgmParticlesVelocityCurr_, 
	//and pcvgmParticleOrbDescriptorsCurr_, moreover, the cvgmSaliencyCurr_
	unsigned int cudaTrackOrb(const unsigned short usMatchThreshold_, const unsigned short usHalfSize_, const short sSearchRange_,
							const cv::gpu::GpuMat& cvgmParticleOrbDescriptorsPrev_, const cv::gpu::GpuMat& cvgmParticleResponsesPrev_, 
							const cv::gpu::GpuMat& cvgmParticleDescriptorCurrTmp_,
							const cv::gpu::GpuMat& cvgmSaliencyCurr_,
							cv::gpu::GpuMat* pcvgmMinMatchDistance_,
							cv::gpu::GpuMat* pcvgmMatchedLocationPrev_);
	//separate salient point into matched and newly added.
	//for matched keypoints the velocity and age will be updated
	void cudaCollectKeyPointOrb(unsigned int uTotalParticles_, unsigned int uMaxNewKeyPoints_, const float fRho_,
										const cv::gpu::GpuMat& cvgmSaliency_,/*const cv::gpu::GpuMat& cvgmParticleResponseCurrTmp_,*/
										const cv::gpu::GpuMat& cvgmParticleDescriptorCurrTmp_,
										const cv::gpu::GpuMat& cvgmParticleVelocityPrev_,
										const cv::gpu::GpuMat& cvgmParticleAgePrev_,
										const cv::gpu::GpuMat& cvgmMinMatchDistance_,
										const cv::gpu::GpuMat& cvgmMatchedLocationPrev_,
										cv::gpu::GpuMat* pcvgmNewlyAddedKeyPointLocation_, cv::gpu::GpuMat* pcvgmNewlyAddedKeyPointResponse_,
										cv::gpu::GpuMat* pcvgmMatchedKeyPointLocation_, cv::gpu::GpuMat* pcvgmMatchedKeyPointResponse_,
										cv::gpu::GpuMat* pcvgmParticleResponseCurr_, cv::gpu::GpuMat* pcvgmParticleDescriptorCurr_,
										cv::gpu::GpuMat* pcvgmParticleVelocityCurr_, cv::gpu::GpuMat* pcvgmParticleAgeCurr_);
	void cudaCollectParticles(const short2* ps2KeyPointsLocations_, const float* pfKeyPointsResponse_, const unsigned int uTotalParticles_, 
								cv::gpu::GpuMat* pcvgmParticleResponses_, cv::gpu::GpuMat* pcvgmParticleDescriptor_, const cv::gpu::GpuMat& cvgmImage_=cv::gpu::GpuMat() );
	unsigned int cudaMatchedAndNewlyAddedKeyPointsCollection(cv::gpu::GpuMat& cvgmKeyPointLocation_, 
																unsigned int* puMaxSalientPoints_, cv::gpu::GpuMat* pcvgmParticleResponsesCurr_, 
																short2* ps2devMatchedKeyPointLocations_, float* pfdevMatchedKeyPointResponse_, 
																short2* ps2devNewlyAddedKeyPointLocations_, float* pfdevNewlyAddedKeyPointResponse_);
}//semidense
}//device
}//btl

btl::image::semidense::CSemiDenseTrackerOrb::CSemiDenseTrackerOrb()
{
	//Gaussian filter
	_fSigma = 1.f; // page3: r=3/6 and sigma = 1.f/2.f respectively
	_uRadius = 3; // 
	_uGaussianKernelSize = 2*_uRadius + 1;
	//contrast threshold
	_ucContrastThresold = 5; // 255 * 0.02 = 5.1

	//saliency threshold
	_fSaliencyThreshold = 0.2f;
	//match threshold
	_usMatchThreshod[0] = 9;
	_usMatchThreshod[1] = 12;
	_usMatchThreshod[2] = 12;
	_usMatchThreshod[3] = 12; 

	//# of Max key points
	_uMaxKeyPointsBeforeNonMax[0] = 80000;
	_uMaxKeyPointsBeforeNonMax[1] = 10000;
	_uMaxKeyPointsBeforeNonMax[2] =  2500;
	_uMaxKeyPointsBeforeNonMax[3] =   650;

	_uMaxKeyPointsAfterNonMax[0] = 20000;
	_uMaxKeyPointsAfterNonMax[1] =  2500;
	_uMaxKeyPointsAfterNonMax[2] =   600;
	_uMaxKeyPointsAfterNonMax[3] =   150;

	_uTotalParticles[0] = 8000;
	_uTotalParticles[1] = 2000;
	_uTotalParticles[2] =  500;
	_uTotalParticles[3] =  100;

	_usHalfPatchSize = 6; //the size of the orb feature
	_sSearchRange = 5;

	_nFrameIdx = 0;

	_uMatchedPoints[0] = 0;
	_uMatchedPoints[1] = 0;
	_uMatchedPoints[2] = 0;
	_uMatchedPoints[3] = 0;
}

void btl::image::semidense::CSemiDenseTrackerOrb::initUMax(){
	// pre-compute the end of a row in a circular patch (1/4 of the circular patch)
	int half_patch_size = _usHalfPatchSize;
	std::vector<int> u_max(half_patch_size + 2);
	for (int v = 0; v <= half_patch_size * std::sqrt(2.f) / 2 + 1; ++v)
		u_max[v] = cvRound(std::sqrt(static_cast<float>(half_patch_size * half_patch_size - v * v)));

	// Make sure we are symmetric
	for (int v = half_patch_size, v_0 = 0; v >= half_patch_size * std::sqrt(2.f) / 2; --v){
		while (u_max[v_0] == u_max[v_0 + 1])
			++v_0;
		u_max[v] = v_0;
		++v_0;
	}
	btl::device::semidense::loadUMax(&u_max[0], static_cast<int>(u_max.size()));
}
void btl::image::semidense::CSemiDenseTrackerOrb::makeRandomPattern(unsigned short usHalfPatchSize_, int nPoints_, cv::Mat* pcvmPattern_)
{
	// we always start with a fixed seed,
	// to make patterns the same on each run
	cv::RNG rng(0x34985739);

	for (int i = 0; i < nPoints_; i++){
		pcvmPattern_->ptr<short>(0)[i] = rng.uniform(- usHalfPatchSize_, usHalfPatchSize_ + 1);
		pcvmPattern_->ptr<short>(1)[i] = rng.uniform(- usHalfPatchSize_, usHalfPatchSize_ + 1);
	}
}
void btl::image::semidense::CSemiDenseTrackerOrb::initOrbPattern(){
	// Calc cvmPattern_
	const int nPoints = 128; // 64 tests and each test requires 2 points 256x2 = 512
	cv::Mat cvmPattern; //2 x n : 1st row is x and 2nd row is y; test point1, test point2;
	//assign cvmPattern_ from precomputed patterns
	cvmPattern.create(2, nPoints, CV_16SC1);
	makeRandomPattern(_usHalfPatchSize, nPoints, &cvmPattern );
	_cvgmPattern.upload(cvmPattern);//2 x n : 1st row is x and 2nd row is y; test point1, test point2;
	return;
}

bool btl::image::semidense::CSemiDenseTrackerOrb::initialize( boost::shared_ptr<cv::gpu::GpuMat> _acvgmShrPtrPyrBW[4] )
{
	_nFrameIdx = 0;
	initUMax();
	initOrbPattern();
	for (int n = 3; n>-1; --n ){
		_cvgmSaliency[n].create(_acvgmShrPtrPyrBW[n]->size(),CV_32FC1);
		_cvgmInitKeyPointLocation[n].create(1, _uMaxKeyPointsBeforeNonMax[n], CV_16SC2);
		_cvgmFinalKeyPointsLocationsAfterNonMax[n].create(1, _uMaxKeyPointsAfterNonMax[n], CV_16SC2);//short2 location;
		_cvgmFinalKeyPointsResponseAfterNonMax[n].create(1, _uMaxKeyPointsAfterNonMax[n], CV_32FC1);//float corner strength(response);  

		_cvgmMatchedKeyPointLocation[n].create(1, _uTotalParticles[n], CV_16SC2);
		_cvgmMatchedKeyPointResponse[n].create(1, _uTotalParticles[n], CV_32FC1);
		_cvgmNewlyAddedKeyPointLocation[n].create(1, _uMaxKeyPointsAfterNonMax[n], CV_16SC2);
		_cvgmNewlyAddedKeyPointResponse[n].create(1, _uMaxKeyPointsAfterNonMax[n], CV_32FC1);

		//init particles
		_cvgmParticleResponsePrev[n].create(_acvgmShrPtrPyrBW[n]->size(),CV_32FC1);	   _cvgmParticleResponsePrev[n].setTo(0);
		_cvgmParticleVelocityPrev[n].create(_acvgmShrPtrPyrBW[n]->size(),CV_16SC2);	   _cvgmParticleVelocityPrev[n].setTo(cv::Scalar::all(0));//float velocity; 
		_cvgmParticleAgePrev[n].create(_acvgmShrPtrPyrBW[n]->size(),CV_8UC1);			   _cvgmParticleAgePrev[n].setTo(0);//uchar age;
		_cvgmParticleDescriptorPrev[n].create(_acvgmShrPtrPyrBW[n]->size(),CV_32SC2);    _cvgmParticleDescriptorPrev[n].setTo(cv::Scalar::all(0));

		_cvgmParticleResponseCurr[n].create(_acvgmShrPtrPyrBW[n]->size(),CV_32FC1);	   _cvgmParticleResponseCurr[n].setTo(0);
		_cvgmParticleAngleCurr[n].create(_acvgmShrPtrPyrBW[n]->size(),CV_32FC1);		   _cvgmParticleAngleCurr[n].setTo(0);
		_cvgmParticleVelocityCurr[n].create(_acvgmShrPtrPyrBW[n]->size(),CV_16SC2);	   _cvgmParticleVelocityCurr[n].setTo(cv::Scalar::all(0));//float velocity; 
		_cvgmParticleAgeCurr[n].create(_acvgmShrPtrPyrBW[n]->size(),CV_8UC1);		       _cvgmParticleAgeCurr[n].setTo(0);//uchar age;
		_cvgmParticleDescriptorCurr[n].create(_acvgmShrPtrPyrBW[n]->size(),CV_32SC2);	   _cvgmParticleDescriptorCurr[n].setTo(cv::Scalar::all(0));
		_cvgmParticleDescriptorCurrTmp[n].create(_acvgmShrPtrPyrBW[n]->size(),CV_32SC2); _cvgmParticleDescriptorCurrTmp[n].setTo(cv::Scalar::all(0));

		_cvgmMinMatchDistance[n].create(_acvgmShrPtrPyrBW[n]->size(),CV_8UC1);
		_cvgmMatchedLocationPrev[n].create(_acvgmShrPtrPyrBW[n]->size(),CV_16SC2);

		//allocate filter
		if (_pBlurFilter.empty()){
			_pBlurFilter = cv::gpu::createGaussianFilter_GPU(CV_8UC1, cv::Size(_uGaussianKernelSize, _uGaussianKernelSize), _fSigma, _fSigma, cv::BORDER_REFLECT_101);
		}
		//processing the frame
		//apply gaussian filter
		_pBlurFilter->apply(*_acvgmShrPtrPyrBW[n], _cvgmBlurredPrev[n], cv::Rect(0, 0, _acvgmShrPtrPyrBW[n]->cols, _acvgmShrPtrPyrBW[n]->rows));
		//detect key points
		//1.compute the saliency score 
		unsigned int uTotalSalientPoints = btl::device::semidense::cudaCalcSaliency(_cvgmBlurredPrev[n], unsigned short(_usHalfPatchSize*1.5) ,_ucContrastThresold, _fSaliencyThreshold, 
																					&_cvgmSaliency[n], &_cvgmInitKeyPointLocation[n]); 
		if (uTotalSalientPoints< 50 ) return false;
		uTotalSalientPoints = std::min( uTotalSalientPoints, _uMaxKeyPointsBeforeNonMax[n] );
		
		//2.do a non-max suppression and initialize particles ( extract feature descriptors ) 
		unsigned int uFinalSalientPointsAfterNonMax = btl::device::semidense::cudaNonMaxSupression(_cvgmInitKeyPointLocation[n], uTotalSalientPoints, _cvgmSaliency[n], 
																								   _cvgmFinalKeyPointsLocationsAfterNonMax[n].ptr<short2>(), _cvgmFinalKeyPointsResponseAfterNonMax[n].ptr<float>() );
		uFinalSalientPointsAfterNonMax = std::min( uFinalSalientPointsAfterNonMax, _uMaxKeyPointsAfterNonMax[n] );
	
		//3.sort all salient points according to their strength and pick the first _uTotalParticles;
		btl::device::semidense::thrustSort(_cvgmFinalKeyPointsLocationsAfterNonMax[n].ptr<short2>(),_cvgmFinalKeyPointsResponseAfterNonMax[n].ptr<float>(),uFinalSalientPointsAfterNonMax);
		_uTotalParticles[n] = std::min( _uTotalParticles[n], uFinalSalientPointsAfterNonMax );

		//4.collect all salient points and descriptors on them
		_cvgmParticleResponsePrev[n].setTo(0.f);
		btl::device::semidense::cudaExtractAllDescriptorOrb(_cvgmBlurredPrev[n],
															_cvgmFinalKeyPointsLocationsAfterNonMax[n].ptr<short2>(),_cvgmFinalKeyPointsResponseAfterNonMax[n].ptr<float>(),
															_uTotalParticles[n],_usHalfPatchSize,
															_cvgmPattern.ptr<short>(0),_cvgmPattern.ptr<short>(1),
															&_cvgmParticleResponsePrev[n], &_cvgmParticleAngleCurr[n], &_cvgmParticleDescriptorPrev[n]);
		/*int nCounter = 0;
		bool bIsLegal = testCountResponseAndDescriptor(_cvgmParticleResponsePrev[n],_cvgmParticleDescriptorPrev[n],&nCounter);
		//test
		cv::gpu::GpuMat cvgmTestResponse(_cvgmParticleResponsePrev[n]); cvgmTestResponse.setTo(0.f);
		cv::gpu::GpuMat cvgmTestOrbDescriptor(_cvgmParticleDescriptorPrev[n]);cvgmTestOrbDescriptor.setTo(cv::Scalar::all(0));
		testCudaCollectParticlesAndOrbDescriptors(_cvgmFinalKeyPointsLocationsAfterNonMax[n],_cvgmFinalKeyPointsResponseAfterNonMax[n],_cvgmBlurredPrev[n],
												 _uTotalParticles[n],_usHalfPatchSize,_cvgmPattern,
									  			 &cvgmTestResponse,&_cvgmParticleAngleCurr[n],&cvgmTestOrbDescriptor);
		float fD1 = testMatDiff(_cvgmParticleResponsePrev[n], cvgmTestResponse);
		float fD2 = testMatDiff(_cvgmParticleDescriptorPrev[n], cvgmTestOrbDescriptor);*/
	
		//store velocity
		_cvgmParticleVelocityPrev[n].download(_cvmKeyPointVelocity[_nFrameIdx][n]);

	}
	
	return true;
}

void btl::image::semidense::CSemiDenseTrackerOrb::track( boost::shared_ptr<cv::gpu::GpuMat> _acvgmShrPtrPyrBW[4] )
{
	btl::other::increase<int>(30,&_nFrameIdx);

	for (int n = 3; n>-1; --n ) {
		//processing the frame
		//Gaussian smoothes the input image 
		_pBlurFilter->apply(*_acvgmShrPtrPyrBW[n] , _cvgmBlurredCurr[n], cv::Rect(0, 0, _acvgmShrPtrPyrBW[n]->cols, _acvgmShrPtrPyrBW[n]->rows));
		//calc the saliency score for each pixel
		unsigned int uTotalSalientPoints = btl::device::semidense::cudaCalcSaliency(_cvgmBlurredCurr[n], unsigned short(_usHalfPatchSize*1.5), _ucContrastThresold, _fSaliencyThreshold, &_cvgmSaliency[n], &_cvgmInitKeyPointLocation[n]);
		uTotalSalientPoints = std::min( uTotalSalientPoints, _uMaxKeyPointsBeforeNonMax[n] );
	
		//do a non-max suppression and collect the candidate particles into a temporary vectors( extract feature descriptors ) 
		unsigned int uFinalSalientPoints = btl::device::semidense::cudaNonMaxSupression(_cvgmInitKeyPointLocation[n], uTotalSalientPoints, _cvgmSaliency[n], 
																						_cvgmFinalKeyPointsLocationsAfterNonMax[n].ptr<short2>(), _cvgmFinalKeyPointsResponseAfterNonMax[n].ptr<float>() );
		_uFinalSalientPoints[n] = uFinalSalientPoints = std::min( uFinalSalientPoints, unsigned int(_uMaxKeyPointsAfterNonMax[n]) );
		_cvgmSaliency[n].setTo(0.f);//clear saliency scores
		//redeploy the saliency matrix
		btl::device::semidense::cudaExtractAllDescriptorOrb(_cvgmBlurredCurr[n],
															_cvgmFinalKeyPointsLocationsAfterNonMax[n].ptr<short2>(),_cvgmFinalKeyPointsResponseAfterNonMax[n].ptr<float>(),
															uFinalSalientPoints,_usHalfPatchSize,
															_cvgmPattern.ptr<short>(0),_cvgmPattern.ptr<short>(1),
															&_cvgmSaliency[n], &_cvgmParticleAngleCurr[n], &_cvgmParticleDescriptorCurrTmp[n]);
		/*
		int nCounter = 0;
		bool bIsLegal = testCountResponseAndDescriptor(_cvgmSaliency[n],_cvgmParticleDescriptorCurrTmp[n],&nCounter);*/

		//track particles in previous frame by searching the candidates of current frame. 
		//Note that _cvgmSaliency is the input as well as output, tracked particles are marked as negative scores
		_cvgmMatchedLocationPrev[n].setTo(cv::Scalar::all(0));
		_uMatchedPoints[n] = btl::device::semidense::cudaTrackOrb( _usMatchThreshod[n], _usHalfPatchSize, _sSearchRange,
																_cvgmParticleDescriptorPrev[n],  _cvgmParticleResponsePrev[n], 
																_cvgmParticleDescriptorCurrTmp[n], _cvgmSaliency[n],
																&_cvgmMinMatchDistance[n],
																&_cvgmMatchedLocationPrev[n]);
		/*
		nCounter = 0;
		bIsLegal = testCountResponseAndDescriptor(_cvgmSaliency[n],_cvgmParticleDescriptorCurrTmp[n],&nCounter);
		nCounter = 0;
		bIsLegal = testCountMinDistAndMatchedLocation( _cvgmMinMatchDistance[n], _cvgmMatchedLocationPrev[n], &nCounter );

		cv::Mat cvmPattern; _cvgmPattern.download(cvmPattern);
		cv::gpu::GpuMat cvgmMinMatchDistanceTest,cvgmMatchedLocationPrevTest;
		cvgmMinMatchDistanceTest       .create(_cvgmBlurredCurr[n].size(),CV_8UC1);
		cvgmMatchedLocationPrevTest    .create(_cvgmBlurredCurr[n].size(),CV_16SC2);	cvgmMatchedLocationPrevTest.setTo(cv::Scalar::all(0));
		unsigned int uMatchedPointsTest = testCudaTrackOrb( _usMatchThreshod[n], _usHalfPatchSize, _sSearchRange,
															_cvgmPattern.ptr<short>(0), _cvgmPattern.ptr<short>(1), 
															_cvgmParticleDescriptorPrev[n], _cvgmParticleResponsePrev[n], 
															_cvgmParticleDescriptorCurrTmp[n],
															_cvgmSaliency[n], &cvgmMinMatchDistanceTest, &cvgmMatchedLocationPrevTest);
		float fD0 = testMatDiff(_cvgmMatchedLocationPrev[n], cvgmMatchedLocationPrevTest);
		float fD1 = testMatDiff(_cvgmMinMatchDistance[n],cvgmMinMatchDistanceTest);
		float fD2 = (float)uMatchedPointsTest - nCounter;
		*/

		//separate tracked particles and rest of candidates. Note that saliency scores are updated 
		//Note that _cvgmSaliency is the input as well as output, after the tracked particles are separated with rest of candidates, their negative saliency
		//scores are recovered into positive scores
		_cvgmMatchedKeyPointLocation   [n].setTo(cv::Scalar::all(0));//clear all memory
		_cvgmMatchedKeyPointResponse   [n].setTo(0.f);
		_cvgmNewlyAddedKeyPointLocation[n].setTo(cv::Scalar::all(0));//clear all memory
		_cvgmNewlyAddedKeyPointResponse[n].setTo(0.f);
		btl::device::semidense::cudaCollectKeyPointOrb(_uTotalParticles[n], _uMaxKeyPointsAfterNonMax[n], 0.75f,
														_cvgmSaliency[n], _cvgmParticleDescriptorCurrTmp[n],
														_cvgmParticleVelocityPrev[n],_cvgmParticleAgePrev[n],
														_cvgmMinMatchDistance[n],_cvgmMatchedLocationPrev[n],
														&_cvgmNewlyAddedKeyPointLocation[n], &_cvgmNewlyAddedKeyPointResponse[n], 
														&_cvgmMatchedKeyPointLocation[n], &_cvgmMatchedKeyPointResponse[n],
														&_cvgmParticleResponseCurr[n], &_cvgmParticleDescriptorCurr[n],
														&_cvgmParticleVelocityCurr[n],&_cvgmParticleAgeCurr[n]);
	/*
		nCounter = 0;
		bIsLegal = testCountResponseAndDescriptor(_cvgmSaliency[n],_cvgmParticleDescriptorCurrTmp[n],&nCounter);

		nCounter = 0;
		bIsLegal = testCountResponseAndDescriptor(_cvgmParticleResponseCurr[n],_cvgmParticleDescriptorCurr[n],&nCounter);
		cv::gpu::GpuMat cvgmNewlyAddedKeyPointLocationTest(_cvgmNewlyAddedKeyPointLocation[n]), cvgmNewlyAddedKeyPointResponseTest(_cvgmNewlyAddedKeyPointResponse[n]), 
						cvgmMatchedKeyPointLocationTest(_cvgmMatchedKeyPointLocation[n]), cvgmMatchedKeyPointResponseTest(_cvgmMatchedKeyPointResponse[n]),
						cvgmParticleResponseCurrTest(_cvgmParticleResponseCurr[n]), cvgmParticleDescriptorCurrTest(_cvgmParticleDescriptorCurr[n]),
						cvgmParticleVelocityCurrTest(_cvgmParticleVelocityCurr[n]), cvgmParticleAgeCurrTest(_cvgmParticleAgeCurr[n]);
	
		nCounter = 0;
		bIsLegal = testCountResponseAndDescriptor(_cvgmSaliency[n], _cvgmParticleDescriptorCurrTmp[n],&nCounter);

		testCudaCollectNewlyAddedKeyPoints(_uTotalParticles[n], _uMaxKeyPointsAfterNonMax[n], 0.75f,
										  _cvgmSaliency[n], _cvgmParticleDescriptorCurrTmp[n],
										  _cvgmParticleVelocityPrev[n],_cvgmParticleAgePrev[n],
										  _cvgmMinMatchDistance[n],_cvgmMatchedLocationPrev[n],
										  &cvgmNewlyAddedKeyPointLocationTest, &cvgmNewlyAddedKeyPointResponseTest, 
										  &cvgmMatchedKeyPointLocationTest, &cvgmMatchedKeyPointResponseTest,
										  &cvgmParticleResponseCurrTest, &cvgmParticleDescriptorCurrTest,
										  &cvgmParticleVelocityCurrTest,&cvgmParticleAgeCurrTest);
		nCounter = 0;
		bIsLegal = testCountResponseAndDescriptor(_cvgmParticleResponseCurr[n],_cvgmParticleDescriptorCurr[n],&nCounter);
		float fD3 = testMatDiff(cvgmNewlyAddedKeyPointLocationTest, _cvgmNewlyAddedKeyPointLocation[n]);
		float fD4 = testMatDiff(cvgmNewlyAddedKeyPointResponseTest, _cvgmNewlyAddedKeyPointResponse[n]);
		float fD5 = testMatDiff(cvgmMatchedKeyPointLocationTest, _cvgmMatchedKeyPointLocation[n]);
		float fD6 = testMatDiff(cvgmMatchedKeyPointResponseTest, _cvgmMatchedKeyPointResponse[n]);
		float fD7 = testMatDiff(cvgmParticleResponseCurrTest, _cvgmParticleResponseCurr[n]);
		float fD8 = testMatDiff(cvgmParticleDescriptorCurrTest, _cvgmParticleDescriptorCurr[n]);
		float fD9 = testMatDiff(cvgmParticleVelocityCurrTest, _cvgmParticleVelocityCurr[n]);
		float fD10= testMatDiff(cvgmParticleAgeCurrTest, _cvgmParticleAgeCurr[n]);
*/


		//h) assign the current frame to previous frame
		_cvgmBlurredCurr		 [n].copyTo(_cvgmBlurredPrev[n]);
		_cvgmParticleResponseCurr[n].copyTo(_cvgmParticleResponsePrev[n]);
		_cvgmParticleAgeCurr	 [n].copyTo(_cvgmParticleAgePrev[n]);
		_cvgmParticleVelocityCurr[n].copyTo(_cvgmParticleVelocityPrev[n]);
		_cvgmParticleDescriptorCurr[n].copyTo(_cvgmParticleDescriptorPrev[n]);

		//store velocity
		_cvgmParticleVelocityCurr[n].download(_cvmKeyPointVelocity[_nFrameIdx][n]);
		//render keypoints
		_cvgmMatchedKeyPointLocation[n].download(_cvmKeyPointLocation[n]);
		_cvgmParticleAgeCurr[n].download(_cvmKeyPointAge[n]);
	}
	
	

	return;
}






