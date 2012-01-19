#ifndef BTL_EXTRA_MODEL
#define BTL_EXTRA_MODEL


namespace btl
{
namespace extra
{
using namespace btl::utility;

class CModel
{
public:
	CModel( VideoSourceKinect& cKinect_ );
	~CModel(void);
	//loaders
	void loadFrame(); //load depth and rgb from video source and convert it to point cloud data
	void loadPyramid(); //load pyramid
	void convert2PointCloudModelGL(const cv::Mat& cvmDepth_,const cv::Mat& cvmRGB_, unsigned int uLevel_, std::vector<const unsigned char*>* vColor_, 
		std::vector<Eigen::Vector3d>* vPt_, std::vector<Eigen::Vector3d>* vNormal_, 
		std::vector< int >* pvX_=NULL, std::vector< int >* pvY_=NULL);
	void detectPlanePCL(unsigned int uLevel_,std::vector<int>* pvXIdx_, std::vector<int>* pvYIdx_);
	void loadPyramidAndDetectPlane();
	void loadPyramidAndDetectPlanePCL();
	//extract a plane from depth map and convert to GL convention point cloud
	void extractPlaneGL(unsigned int uLevel_, const std::vector<int>& vX_, const std::vector<int>& vY_, std::vector<Eigen::Vector3d>* pvPlane_);
	void clusterNormal();
public:
	//global model
	std::vector< Eigen::Vector3d > _vGlobalPts;
	std::vector< Eigen::Vector3d > _vGlobalNormals;
	std::vector<const unsigned char*> _vGlobalColors;
	Eigen::Vector3d	_eivGlobalCentrod;

	//pyramid model (GL-convention)r
	std::vector< std::vector< Eigen::Vector3d > > _vvPyramidPts; 
	std::vector< std::vector< Eigen::Vector3d > >_vvPyramidNormals;
	std::vector< std::vector<const unsigned char*> > _vvPyramidColors;
	std::vector< std::vector<int> > _vvX;//x index for non zero PyramidPts, Normals and Colors
	std::vector< std::vector<int> > _vvY;//y index
	std::vector< std::vector< unsigned int > > _vvNormalIdx;//idx of sampling the unit half sphere of top pyramid
	std::vector< std::vector< unsigned int > > _vvLabelNormalIdx;//normal idx associated with each cluster label
	std::vector< Eigen::Vector3d > _vLabelAvgNormals;
	//planes
	std::vector< Eigen::Vector3d > _veivPlane;
private:
	//frame data
	Eigen::Vector3d _eivCentroid;
	//frame pyramid
	std::vector< cv::Mat > _vcvmPyramidRGBs;
	std::vector< cv::Mat > _vcvmPyramidDepths;

	//frame index
	unsigned int _uCurrent;
	// refreshed for every frame
	//X,Y,Z coordinate of depth w.r.t. RGB camera reference system
	//in the format of the RGB image
	double* _pPointL0;
	
	//video source
	btl::extra::videosource::VideoSourceKinect& _cKinect;
public:
	//control
	int _nKNearest; // for normal extraction using PCL
	enum {  _FAST, _PCL } _eNormalExtraction;
};

}//extra
}//btl
#endif
