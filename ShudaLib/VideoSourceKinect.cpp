/**
* @file VideoSourceKinect.cpp
* @brief load of data from a kinect device 
* @author Shuda Li<lishuda1980@gmail.com>
* @version 1.0
* @date 2011-02-23
*/
#define INFO
#define TIMER

#include <opencv2/gpu/gpu.hpp>
#include "VideoSourceKinect.hpp"
#include "Utility.hpp"
#include "cuda/CudaLib.h"

#include <iostream>
#include <string>


#define CHECK_RC(rc, what)	\
	BTL_ASSERT(rc == XN_STATUS_OK, (what + std::string(xnGetStatusString(rc))) )

using namespace btl::utility;

namespace btl{ namespace extra { namespace videosource
{

VideoSourceKinect::VideoSourceKinect ()
:CCalibrateKinect()
{
    std::cout << "  VideoSourceKinect: Opening Kinect..." << std::endl;

    XnStatus nRetVal = XN_STATUS_OK;
    //initialize OpenNI context
/*    
    nRetVal = _cContext.InitFromXmlFile("/space/csxsl/src/btl-shuda/Kinect.xml"); 
    CHECK_RC ( nRetVal, "Initialize context: " );
    nRetVal = _cContext.FindExistingNode(XN_NODE_TYPE_IR, _cIRGen); 
    CHECK_RC ( nRetVal, "Find existing node: " );
*/    
    nRetVal = _cContext.Init();
    CHECK_RC ( nRetVal, "Initialize context: " );

    //create a image generator
    nRetVal =  _cImgGen.Create ( _cContext );
    CHECK_RC ( nRetVal, "Create image generator: " );
    //create a depth generator
    nRetVal =  _cDepthGen.Create ( _cContext );
    CHECK_RC ( nRetVal, "Create depth generator: " );
    //start generating data
    nRetVal = _cContext.StartGeneratingAll();
    CHECK_RC ( nRetVal, "Start generating data: " );
	//register the depth generator with the image generator
    //nRetVal = _cDepthGen.GetAlternativeViewPointCap().SetViewPoint ( _cImgGen );
	//CHECK_RC ( nRetVal, "Getting and setting AlternativeViewPoint failed: " ); 
	PRINTSTR("Kinect connected");
    _cvmRGB			   .create( KINECT_HEIGHT, KINECT_WIDTH, CV_8UC3 );
	_cvmUndistRGB	   .create( KINECT_HEIGHT, KINECT_WIDTH, CV_8UC3 );
	_cvmDepth		   .create( KINECT_HEIGHT, KINECT_WIDTH, CV_32FC1);
	_cvmUndistDepth	   .create( KINECT_HEIGHT, KINECT_WIDTH, CV_32FC1);
	_cvmAlignedRawDepth.create( KINECT_HEIGHT, KINECT_WIDTH, CV_32FC1);

	_cvmIRWorld .create(KINECT_HEIGHT,KINECT_WIDTH,CV_32FC3);
	_cvmRGBWorld.create(KINECT_HEIGHT,KINECT_WIDTH,CV_32FC3);

	// allocate memory for later use ( registrate the depth with rgb image
	// refreshed for every frame
	// pre-allocate cvgm to increase the speed
	_cvgmIRWorld        .create(KINECT_HEIGHT,KINECT_WIDTH,CV_32FC3);
	_cvgmRGBWorld       .create(KINECT_HEIGHT,KINECT_WIDTH,CV_32FC3);
	_cvgmAlignedRawDepth.create(KINECT_HEIGHT,KINECT_WIDTH,CV_32FC1);
	_cvgm32FC1Tmp       .create(KINECT_HEIGHT,KINECT_WIDTH,CV_32FC1);
	PRINTSTR("data holder constructed...");
	//disparity
	for(int i=0; i<4; i++)
	{
		int nRows = KINECT_HEIGHT>>i; 
		int nCols = KINECT_WIDTH>>i;
		//device
		_vcvgmPyrDepths    .push_back(cv::gpu::GpuMat(nRows,nCols,CV_32FC1));
		_vcvgmPyrDisparity .push_back(cv::gpu::GpuMat(nRows,nCols,CV_32FC1));
		_vcvgmPyrRGBs      .push_back(cv::gpu::GpuMat(nRows,nCols,CV_8UC3));
		_vcvgmPyr32FC1Tmp  .push_back(cv::gpu::GpuMat(nRows,nCols,CV_32FC1));
		_vcvgmPyrPts	   .push_back(cv::gpu::GpuMat(nRows,nCols,CV_32FC3));
		_vcvgmPyrNls	   .push_back(cv::gpu::GpuMat(nRows,nCols,CV_32FC3));
		//host
		_vcvmPyrDepths.push_back(cv::Mat(nRows,nCols,CV_32FC1));
		_vcvmPyrRGBs  .push_back(cv::Mat(nRows,nCols,CV_8UC3 ));
		_acvmShrPtrPyrPts[i].reset(new cv::Mat(nRows,nCols,CV_32FC3));
		_acvmShrPtrPyrNls[i].reset(new cv::Mat(nRows,nCols,CV_32FC3));
		_acvmShrPtrPyrRGBs[i].reset(new cv::Mat(nRows,nCols,CV_8UC3));
		PRINTSTR("construct pyrmide level:");
		PRINT(i);
	}

	//other
    _ePreFiltering = GPU_PYRAMID; 
	//definition of parameters
	_fThresholdDepthInMeter = 0.03f;
	_fSigmaSpace = 2;
	_fSigmaDisparity = 1.f/.6f - 1.f/(.6f+_fThresholdDepthInMeter);
	_uPyrHeight = 1;

	//default centroid follows opencv-convention
	_dXCentroid = _dYCentroid = 0;
	_dZCentroid = 1.0;

	std::cout << " Done. " << std::endl;
}
VideoSourceKinect::~VideoSourceKinect()
{
    _cContext.Release();
}
void VideoSourceKinect::findRange(const cv::gpu::GpuMat& cvgmMat_)
{
	cv::Mat cvmMat;
	cvgmMat_.download(cvmMat);
	findRange(cvmMat);
}
void VideoSourceKinect::findRange(const cv::Mat& cvmMat_)
{
	//BTL_ASSERT(cvmMat_.type()==CV_32F,"findRange() must be CV_32F");
	int N,s;
	switch(cvmMat_.type())
	{
	case CV_32F:
		N = cvmMat_.cols*cvmMat_.rows;
		s = 1;
		break;
	case CV_32FC3:
		N = cvmMat_.cols*cvmMat_.rows*3;
		s = 3;
		break;
	}
	float* pData = (float*) cvmMat_.data;
	float fMin =  1.0e+20f;
	float fMax = -1.0e+20f;
	for( int i=s-1; i< N; i+=s)
	{
		float tmp = *pData ++;
		fMin = fMin > tmp? tmp : fMin;
		fMax = fMax < tmp? tmp : fMax;
	}
	PRINT(fMax);
	PRINT(fMin);
	return;
}
void VideoSourceKinect::getNextFrame(tp_frame ePreFiltering_)
{
    //get next frame
#ifdef TIMER	
	// timer on
	_cT0 =  boost::posix_time::microsec_clock::local_time(); 
#endif

    XnStatus nRetVal = _cContext.WaitAndUpdateAll();
    CHECK_RC ( nRetVal, "UpdateData failed: " );
	// these two lines are required for getting a stable image and depth.
    _cImgGen.GetMetaData ( _cImgMD );
    _cDepthGen.GetMetaData( _cDepthMD );

    const XnRGB24Pixel* pRGBImg  = _cImgMD.RGB24Data();
	     unsigned char* pRGB = _cvmRGB.data;
    const unsigned short* pDepth   = (unsigned short*)_cDepthMD.Data();
	      float* pcvDepth = (float*)_cvmDepth.data;
    
    //XnStatus nRetVal = _cContext.WaitOneUpdateAll( _cIRGen );
    //CHECK_RC ( nRetVal, "UpdateData failed: " );
		  
	for( unsigned int i = 0; i < __aKinectWxH[0]; i++,pRGBImg++){
        // notice that OpenCV is use BGR order
        *pRGB++ = uchar(pRGBImg->nRed);
        *pRGB++ = uchar(pRGBImg->nGreen);
        *pRGB++ = uchar(pRGBImg->nBlue);
		*pcvDepth++ = *pDepth++;
    }
    switch( ePreFiltering_ ){
		case CPU_PYRAMID:
			// not fullly understand the lense distortion model used by OpenNI.
			undistortRGB( _cvmRGB, &_vcvmPyrRGBs[0] );
			undistortIR( _cvmDepth, &_cvmUndistDepth );
			alignDepthWithRGB2(_cvmUndistDepth,&_vcvmPyrDepths[0]);
			btl::utility::bilateralFilterInDisparity<float>(&_vcvmPyrDepths[0],_fSigmaDisparity,_fSigmaSpace);
			unprojectRGBGL(_vcvmPyrDepths[0],0, &*_acvmShrPtrPyrPts[0]);
			fastNormalEstimation(*_acvmShrPtrPyrPts[0],&*_acvmShrPtrPyrNls[0]);
			for( unsigned int i=1; i<_uPyrHeight; i++ )	{
				cv::pyrDown(_vcvmPyrRGBs[i-1],_vcvmPyrRGBs[i]);
				btl::utility::downSampling<float>(_vcvmPyrDepths[i-1],&_vcvmPyrDepths[i]);
				btl::utility::bilateralFilterInDisparity<float>(&_vcvmPyrDepths[i],_fSigmaDisparity,_fSigmaSpace);
				unprojectRGBGL(_vcvmPyrDepths[i],i, &*_acvmShrPtrPyrPts[i]);
				fastNormalEstimation(*_acvmShrPtrPyrPts[i],&*_acvmShrPtrPyrNls[i]);
			}
			break;
		case GPU_PYRAMID:
			_cvgmRGB.upload(_cvmRGB);
			_cvgmDepth.upload(_cvmDepth);
			gpuUndistortRGB(_cvgmRGB,&(_vcvgmPyrRGBs[0]));
			gpuUndistortIR (_cvgmDepth,&_cvgmUndistDepth);
			gpuAlignDepthWithRGB( _cvgmUndistDepth, &_cvgmAlignedRawDepth );
			btl::cuda_util::cudaDepth2Disparity(_cvgmAlignedRawDepth, &_vcvgmPyr32FC1Tmp[0]);
			btl::cuda_util::cudaBilateralFiltering(_vcvgmPyr32FC1Tmp[0],_fSigmaSpace,_fSigmaDisparity,&_vcvgmPyrDisparity[0]);
			btl::cuda_util::cudaDisparity2Depth(_vcvgmPyrDisparity[0],&_vcvgmPyrDepths[0]);
			btl::cuda_util::cudaUnprojectRGBGL(_vcvgmPyrDepths[0],_fFxRGB,_fFyRGB,_uRGB,_vRGB, 0,&_vcvgmPyrPts[0]);
			btl::cuda_util::cudaFastNormalEstimationGL(_vcvgmPyrPts[0],&_vcvgmPyrNls[0]);
			_vcvgmPyrRGBs[0].download(_vcvmPyrRGBs[0]);
			_vcvgmPyrPts[0] .download(*_acvmShrPtrPyrPts[0]);
			_vcvgmPyrNls[0] .download(*_acvmShrPtrPyrNls[0]);
			for( unsigned int i=1; i<_uPyrHeight; i++ )	{
				cv::gpu::pyrDown(_vcvgmPyrRGBs[i-1],_vcvgmPyrRGBs[i]);
				_vcvgmPyrRGBs[i].download(_vcvmPyrRGBs[i]);
				btl::cuda_util::cudaPyrDown( _vcvgmPyrDisparity[i-1],_fSigmaDisparity,&_vcvgmPyr32FC1Tmp[i]);
				btl::cuda_util::cudaBilateralFiltering(_vcvgmPyr32FC1Tmp[i],_fSigmaSpace,_fSigmaDisparity,&(_vcvgmPyrDisparity[i]));
				btl::cuda_util::cudaDisparity2Depth(_vcvgmPyrDisparity[i],&(_vcvgmPyrDepths[i]));
				btl::cuda_util::cudaUnprojectRGBGL(_vcvgmPyrDepths[i],_fFxRGB,_fFyRGB,_uRGB,_vRGB, i,&_vcvgmPyrPts[i]);
				_vcvgmPyrPts[i].download(*_acvmShrPtrPyrPts[i]);
				btl::cuda_util::cudaFastNormalEstimationGL(_vcvgmPyrPts[i],&_vcvgmPyrNls[i]);
				_vcvgmPyrNls[i].download(*_acvmShrPtrPyrNls[i]);
			}
			break;
    }
#ifdef TIMER
// timer off
	_cT1 =  boost::posix_time::microsec_clock::local_time(); 
 	_cTDAll = _cT1 - _cT0 ;
	_fFPS = 1000.f/_cTDAll.total_milliseconds();
	PRINT( _fFPS );
#endif
	//cout << " getNextFrame() ends."<< endl;
    return;
}
void VideoSourceKinect::gpuAlignDepthWithRGB( const cv::gpu::GpuMat& cvgmUndistortDepth_ , cv::gpu::GpuMat* pcvgmAligned_)
{
	pcvgmAligned_->setTo(0);

	//unproject the depth map to IR coordinate
	btl::cuda_util::cudaUnprojectIR(cvgmUndistortDepth_, _fFxIR,_fFyIR,_uIR,_vIR, &_cvgmIRWorld);
	//findRange(_cvgmIRWorld);
	//transform from IR coordinate to RGB coordinate
	btl::cuda_util::cudaTransformIR2RGB(_cvgmIRWorld, _aR, _aRT, &_cvgmRGBWorld);
	//findRange(_cvgmRGBWorld);
	//project RGB coordinate to image to register the depth with rgb image
	//cv::gpu::GpuMat cvgmAligned_(cvgmUndistortDepth_.size(),CV_32FC1);
	btl::cuda_util::cudaProjectRGB(_cvgmRGBWorld, _fFxRGB, _fFyRGB, _uRGB, _vRGB, pcvgmAligned_ );
	//findRange(*pcvgmAligned_);
	return;
}
void VideoSourceKinect::alignDepthWithRGB2( const cv::Mat& cvUndistortDepth_ , cv::Mat* pcvAligned_)
{
	// initialize the Registered depth as NULLs
	pcvAligned_->setTo(0);
	//unproject the depth map to IR coordinate
	unprojectIR      ( cvUndistortDepth_, &_cvmIRWorld );
	//transform from IR coordinate to RGB coordinate
	transformIR2RGB  ( _cvmIRWorld, &_cvmRGBWorld );
	//project RGB coordinate to image to register the depth with rgb image
	projectRGB       ( _cvmRGBWorld,&(*pcvAligned_) );
	return;
}
void VideoSourceKinect::unprojectIR ( const cv::Mat& cvmDepth_, cv::Mat* pcvmIRWorld_)
{
	float* pWorld_ = (float*) pcvmIRWorld_->data;
	float fZ;
	const float* pCamera_=  (const float*) cvmDepth_.data;
	for(int r = 0; r<cvmDepth_.rows; r++)
	for(int c = 0; c<cvmDepth_.cols; c++)
	{
		if ( 400.f < *pCamera_ && *pCamera_ < 3000.f){
			* ( pWorld_ + 2 ) = ( *pCamera_ ) / 1000.f; //convert to meter z 5 million meter is added according to experience. as the OpenNI
			//coordinate system is defined w.r.t. the camera plane which is 0.5 centimeters in front of the camera center
			*   pWorld_		  = ( c - _uIR ) / _fFxIR * *(pWorld_+2); // + 0.0025;     //x by experience.
			* ( pWorld_ + 1 ) = ( r - _vIR ) / _fFyIR * *(pWorld_+2); // - 0.00499814; //y the value is esimated using CCalibrateKinectExtrinsics::calibDepth(
		}
		else{
			*(pWorld_) = *(pWorld_+1) = *(pWorld_+2) = 0;
		}
		pCamera_ ++;
		pWorld_ += 3;
	}
}
void VideoSourceKinect::transformIR2RGB  ( const cv::Mat& cvmIRWorld, cv::Mat* pcvmRGBWorld_ ){
	//_aR[0] [1] [2]
	//   [3] [4] [5]
	//   [6] [7] [8]
	//_aT[0]
	//   [1]
	//   [2]
	//  pRGB_ = _aR * ( pIR_ - _aT )
	//  	  = _aR * pIR_ - _aR * _aT
	//  	  = _aR * pIR_ - _aRT

	float* pRGB_ = (float*) pcvmRGBWorld_->data;
	const float* pIR_=  (float*) cvmIRWorld.data;
	for(int r = 0; r<cvmIRWorld.rows; r++)
	for(int c = 0; c<cvmIRWorld.cols; c++){
		if( 0.4f < fabsf( * ( pIR_ + 2 ) ) && fabsf( * ( pIR_ + 2 ) ) < 3.f ) {
			* pRGB_++ = _aR[0] * *pIR_ + _aR[1] * *(pIR_+1) + _aR[2] * *(pIR_+2) - _aRT[0];
			* pRGB_++ = _aR[3] * *pIR_ + _aR[4] * *(pIR_+1) + _aR[5] * *(pIR_+2) - _aRT[1];
			* pRGB_++ = _aR[6] * *pIR_ + _aR[7] * *(pIR_+1) + _aR[8] * *(pIR_+2) - _aRT[2];
		}
		else {
			* pRGB_++ = 0;
			* pRGB_++ = 0;
			* pRGB_++ = 0;
		}
		pIR_ += 3;
	}
}
void VideoSourceKinect::projectRGB ( const cv::Mat& cvmRGBWorld_, cv::Mat* pcvAlignedRGB_ ){
	//cout << "projectRGB() starts." << std::endl;
	unsigned short nX, nY;
	int nIdx1,nIdx2;
	float dX,dY,dZ;

	CHECK( CV_32FC1 == pcvAlignedRGB_->type(), "the depth pyramid level 1 must be CV_32FC1" );
	float* pWorld_ = (float*) cvmRGBWorld_.data;
	float* pDepth = (float*) pcvAlignedRGB_->data;

	for ( int i = 0; i < KINECT_WxH; i++ ){
		dX = *   pWorld_;
		dY = * ( pWorld_ + 1 );
		dZ = * ( pWorld_ + 2 );
		if (0.4 < fabsf( dZ ) && fabsf( dZ ) < 3){
			// get 2D image projection in RGB image of the XYZ in the world
			nX = cvRound( _fFxRGB * dX / dZ + _uRGB );
			nY = cvRound( _fFyRGB * dY / dZ + _vRGB );
			// set 2D rgb XYZ
			if ( nX >= 0 && nX < KINECT_WIDTH && nY >= 0 && nY < KINECT_HEIGHT ){
				nIdx1= nY * KINECT_WIDTH + nX; //1 channel
				nIdx2= ( nIdx1 ) * 3; //3 channel
				pDepth    [ nIdx1   ] = float(dZ);
				//PRINT( nX ); PRINT( nY ); PRINT( pWorld_ );
			}
		}
		pWorld_ += 3;
	}
	return;
}
void VideoSourceKinect::unprojectRGBGL ( const cv::Mat& cvmDepth_, int nLevel, cv::Mat* pcvmPts_, bool bGLConvertion_/*=true*/ )
{
	pcvmPts_->setTo(0);
	// pCamer format
	// 0 x (c) 1 y (r) 2 d
	//the pixel coordinate is defined w.r.t. camera reference, which is defined as x-left, y-downward and z-forward. It's
	//a right hand system. i.e. opencv-default reference system;
	//unit is meter
	//when rendering the point using opengl's camera reference which is defined as x-left, y-upward and z-backward. the
	//for example: glVertex3d ( Pt(0), -Pt(1), -Pt(2) ); i.e. opengl-default reference system
	int nScale = 1 << nLevel;

	float *pDepth = (float*) cvmDepth_.data;
	float *pWorld_ = (float*) pcvmPts_->data;
	float fD;
	for ( int r = 0; r < cvmDepth_.rows; r++ )
	for ( int c = 0; c < cvmDepth_.cols; c++ )	{
		fD = *pDepth++;
		if( 0.4 < fabsf( fD ) && fabsf( fD ) < 3 ){
			* ( pWorld_ + 2 ) = fD;
			*   pWorld_		  = ( c*nScale - _uRGB ) / _fFxRGB * fD; // + 0.0025;     //x by experience.
			* ( pWorld_ + 1 ) = ( r*nScale - _vRGB ) / _fFyRGB * fD; // - 0.00499814; //y the value is esimated using CCalibrateKinectExtrinsics::calibDepth(
			//convert from opencv convention to opengl convention
			if (bGLConvertion_){
				* ( pWorld_ + 1 ) = -*( pWorld_ + 1 );
				* ( pWorld_ + 2 ) = -*( pWorld_ + 2 );
			}
		}
		else{
			* pWorld_ = *(pWorld_+1) = *(pWorld_+2) = 0.f;
		}
		pWorld_ += 3;
	}
	return;
}
void VideoSourceKinect::clonePyramid(std::vector<cv::Mat>* pvcvmRGB_, std::vector<cv::Mat>* pvcvmDepth_)
{
	if (pvcvmRGB_)
	{
		pvcvmRGB_->clear();
		for(unsigned int i=0; i<_vcvmPyrRGBs.size(); i++)
		{
			pvcvmRGB_->push_back(_vcvmPyrRGBs[i].clone());
		}
	}
	if (pvcvmDepth_)
	{
		pvcvmDepth_->clear();
		for(unsigned int i=0; i<_vcvmPyrDepths.size(); i++)
		{
			pvcvmDepth_->push_back(_vcvmPyrDepths[i].clone());
		}
	}
	return;
}
void VideoSourceKinect::cloneRawFrame( cv::Mat* pcvmRGB_, cv::Mat* pcvmDepth_ )
{
	if (pcvmRGB_)
	{
		*pcvmRGB_ = _vcvmPyrRGBs[0].clone();
	}
	if (pcvmDepth_)
	{
		*pcvmDepth_ = _cvmAlignedRawDepth.clone();
	}
}
void VideoSourceKinect::fastNormalEstimation(const cv::Mat& cvmPts_, cv::Mat* pcvmNls_)
{
	pcvmNls_->setTo(0);
	Eigen::Vector3d n1, n2, n3;

	const float* pPt_ = (const float*) cvmPts_.data;
	float* pNl_ = (float*) pcvmNls_->data;
	//calculate normal
	for( int r = 0; r < cvmPts_.rows; r++ )
	for( int c = 0; c < cvmPts_.cols; c++ )
	{
		if (c == cvmPts_.cols-1 || r == cvmPts_.rows-1) {
			pPt_+=3;
			pNl_+=3;
			continue;
		}
		if ( c == 420 && r == 60 ) {
			int x = 0;
		}
		// skip the right and bottom boarder line
		Eigen::Vector3d pti  ( pPt_[0],pPt_[1],pPt_[2] );
		Eigen::Vector3d pti1 ( pPt_[3],pPt_[4],pPt_[5] ); //left
		Eigen::Vector3d ptj1 ( pPt_[cvmPts_.cols*3],pPt_[cvmPts_.cols*3+1],pPt_[cvmPts_.cols*3+2] ); //down

		if( fabs( pti(2) ) > 0.0000001 && fabs( pti1(2) ) > 0.0000001 && fabs( ptj1(2) ) > 0.0000001 ) {
			n1 = pti1 - pti;
			n2 = ptj1 - pti;
			n3 = n1.cross(n2);
			double dNorm = n3.norm() ;
			if ( dNorm > SMALL ) {
				n3/=dNorm;
				if ( -n3(0)*pti[0] - n3(1)*pPt_[1] - n3(2)*pPt_[2] <0 ) {
					pNl_[0] = -n3(0);
					pNl_[1] = -n3(1);
					pNl_[2] = -n3(2);
				}
				else{
					pNl_[0] = n3(0);
					pNl_[1] = n3(1);
					pNl_[2] = n3(2);
				}
			}//if dNorm
		} //if

		pPt_+=3;
		pNl_+=3;
	}
	return;
}







} //namespace videosource
} //namespace extra
} //namespace btl
