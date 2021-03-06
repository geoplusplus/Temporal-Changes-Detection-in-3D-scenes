#include "utilIO.hpp"
#include "pbaDataInterface.h"
#include "meshProcess.hpp"
#include "../common/globVariables.hpp"

#include <pcl/filters/voxel_grid.h>
#include <pcl/filters/voxel_grid_occlusion_estimation.h>
#include <opencv2/imgproc/imgproc.hpp>

#include <fstream>
#include <sstream>
#include <cstdlib>
#include "opencv2/calib3d/calib3d.hpp"
#include <ctime>


void MeshIO::saveOldModelAsPCL(const std::vector<PtCamCorr> &feat_vec, const std::string &filename){

  vector<vcg::Point3f> out_pts_vect;
  vector<vcg::Color4b> pts_colors;
  vector<vector<vcg::Point3f> > mask_pts;

  for(int i = 0 ; i < feat_vec.size() ; i ++){
    cv::Point3i c = feat_vec[i].ptc;
    out_pts_vect.push_back(feat_vec[i].pts_3d);
    pts_colors.push_back(vcg::Color4b(c.x, c.y, c.z, 0));
  }

  mask_pts.push_back(out_pts_vect);
  MeshIO::saveChngMask3d(mask_pts, pts_colors, filename);
}

/**
   Function creates a mesh from 3D change mask points and saves the PLY file.
*/
void MeshIO::saveChngMask3d(const std::vector<std::vector<vcg::Point3f> > &pts_3d, const std::vector<vcg::Color4b> &pts_colors, const std::string &name){
  
  std::cout<<"Saving change 3D mask.."<<std::endl;
  int colors_size = pts_colors.size();
  MyMesh m;
  float x, y, z;
  int count = 0;

  for(int i = 0 ; i < pts_3d.size() ; i++){
    for(int j = 0 ; j < pts_3d[i].size(); j++){
      x = pts_3d[i].at(j).X();
      y = pts_3d[i].at(j).Y();
      z = pts_3d[i].at(j).Z();
      
      if(colors_size>0)
	vcg::tri::Allocator<MyMesh>::AddVertex(m,
					       MyMesh::CoordType(x,y,z),
					       pts_colors[j]);
      else{
	vcg::tri::Allocator<MyMesh>::AddVertex(m, MyMesh::CoordType(x,y,z));	
	m.vert[count++].SetS();
      }
    }
  }
  if(colors_size<0)
    vcg::tri::UpdateColor<MyMesh>::PerVertexConstant(m, vcg::Color4b::Red, true);
  std::cout<<"Vertices:"<<m.VN()<<std::endl;
  if(m.VN()>0)
    savePlyFileVcg(name,m);
}

/**
   Function returns K-nearest neighbors camera images for given search point
*/
int ImgIO::getKNNcamData(const pcl::KdTreeFLANN<pcl::PointXYZ> &kdtree, pcl::PointXYZ &searchPoint, const std::vector<std::string> &filenames, std::vector<cv::Mat> &out_imgs, int K, std::vector<int> &pointIdxNKNSearch){

  std::vector<float> pointNKNSquaredDistance(K);
  int out = 0;
  
  if(kdtree.nearestKSearch (searchPoint, K, pointIdxNKNSearch, pointNKNSquaredDistance)>0){
    for(int i = 0 ; i < K ; i++){
      out_imgs.push_back(getImg(filenames[pointIdxNKNSearch[i]]) );
    }
    out = 1;
  }
  return out;
}

/**
   Function displays all the images given in the input vector.
*/
void ImgIO::dispImgs(const std::vector<cv::Mat>& inImgs){
  
  std::ostringstream tmpString;
  int moveFactor = 200;

  for(int i = 0 ; i < inImgs.size(); i++){
    tmpString<<i;

    cv::namedWindow(tmpString.str(), cv::WINDOW_NORMAL);
    cv::moveWindow(tmpString.str(), moveFactor, 0);
    cv::imshow(tmpString.str(), inImgs[i]);

    tmpString.flush();
    moveFactor+=moveFactor;
  }
  cv::waitKey(0);                   
}
/**
   Function extracts points from the binary change mask.
*/
void ImgIO::getPtsFromMask(const cv::Mat &mask, std::vector<cv::Point2f> &pts_vector){

  int rows = mask.rows;
  int cols = mask.cols;
  
  for(int r = 0; r < rows; r++){
    for(int c = 0; c < cols; c++){      
      if(mask.at<uchar>(r,c)>0){
	pts_vector.push_back(cv::Point2f(c,r));
      }
    }
  }
}

/**
   Function extracts rotation translation matrix [R | t] from the VCG shot structure into OpenCV Mat structure
*/
cv::Mat ImgIO::getRtMatrix(const vcg::Shot<float> &shot){


  cv::Mat mat_Rt(3,4, CV_64FC1);
  mat_Rt = cv::Mat::zeros(3,4, CV_64FC1);

  vcg::Matrix44f mat_rot = shot.GetWorldToExtrinsicsMatrix();
  
  for(int i = 0 ; i < 3 ; i++){
    for(int j = 0 ; j < 4 ; j++){
      mat_Rt.at<double>(i,j) = static_cast<double>(mat_rot[i][j]);
    }   
  }
 
  return mat_Rt;
}

/**
   Function extracts intrinsic matrix from VCG shot structure into OpenCV Mat structure.
*/
cv::Mat ImgIO::getIntrMatrix(const vcg::Shot<float> &shot){

  cv::Mat intr_mat;
  intr_mat = cv::Mat::zeros(3,3, CV_64FC1);

  intr_mat.at<double>(0,0) = shot.Intrinsics.FocalMm/shot.Intrinsics.PixelSizeMm[0];
  intr_mat.at<double>(1,1) = shot.Intrinsics.FocalMm/shot.Intrinsics.PixelSizeMm[1];
  intr_mat.at<double>(0,2) = shot.Intrinsics.CenterPx[0];
  intr_mat.at<double>(1,2) = shot.Intrinsics.CenterPx[1];
  intr_mat.at<double>(2,2) = 1;

  return intr_mat;
}
/**
Function projects 2D change mask into 3D using point correspondence between SIFT features and model 3D points.
*/
std::vector<vcg::Point3f> ImgIO::projChngMaskCorr(const cv::Mat &chng_mask, const std::vector<ImgFeature> &img_feats, const std::vector<PtCamCorr> &pts_corr, std::set<int> &out_idx){

  cv::Mat mask_copy1(chng_mask.clone());
  cv::Mat mask_copy;

  if(mask_copy1.type()!=CV_8UC1){
    //    mask_copy1.convertTo(mask_copy, CV_8UC1);
    cv::cvtColor(mask_copy1, mask_copy, CV_BGR2GRAY);
  }
  else
    mask_copy = mask_copy1;

  //Vector of result 3D points
  std::vector<vcg::Point3f> out_pts;
  //Iterate through features present in given image
  for(int i = 0 ; i < img_feats.size(); i++){

    //Extract the feature
    ImgFeature tmp_feat;
    tmp_feat = img_feats[i];

    //Check if feature lies under change area in the change mask
    if(mask_copy.at<uchar>(tmp_feat.y+chng_mask.rows/2, tmp_feat.x+chng_mask.cols/2) > 0){    
      out_pts.push_back(pts_corr[tmp_feat.idx].pts_3d);
      out_idx.insert(tmp_feat.idx);
    }
  }
  return out_pts;
}

/**
   Function projects 2D change mask into 3-dimensional space using point cloud voxelization and computation of ray intersections with the voxels
*/
std::vector<vcg::Point3f> ImgIO::projChngMask(const std::string &filename, const cv::Mat &chng_mask, const vcg::Shot<float> &shot, double resolution){
  
  std::cout<<"Projecting 2D change mask into 3D space using ray shooting..." <<std::endl;
  std::vector<vcg::Point3f> out_pts;
  std::vector<cv::Point2f> mask_pts;
  rayBox voxel_grid;
  Eigen::Vector4f origin;
  Eigen::Vector4f origin2;
  vcg::Point3f tmp_pt;
  double prog_perc = 0;

  pcl::PointCloud<pcl::PointXYZ>::Ptr cloud(new pcl::PointCloud<pcl::PointXYZ>);
  
  MeshIO::getPlyFilePCL(filename, cloud);

  shot.Extrinsics.Tra().ToEigenVector(origin2);

  voxel_grid.setInputCloud(cloud);
  voxel_grid.setLeafSize (resolution, resolution, resolution);
  voxel_grid.initializeVoxelGrid();
 
  voxel_grid.setSensorOrigin(origin2);

  getPtsFromMask(chng_mask, mask_pts);

  shot.Extrinsics.Tra().ToEigenVector(origin);  
    
  for(int i = 0 ; i < mask_pts.size(); i++){
    
    if(i % 100 == 0){
      prog_perc = double(i)/double(mask_pts.size());
      DrawProgressBar(40, prog_perc);
    }

    Eigen::Vector4f direction;
    vcg::Point3f tmp_dir = shot.UnProject(vcg::Point2f(mask_pts[i].y, mask_pts[i].x), 100);

    tmp_dir.ToEigenVector(direction);

    float tmp_mp = voxel_grid.getBoxIntersection(origin, direction);
      
    if(tmp_mp == -1.0f){
      continue;
    }
    
    int cloud_idx = -1;

    cloud_idx = voxel_grid.getFirstOccl(origin, direction, tmp_mp);
    
    if(cloud_idx!=-1){
      pcl::PointXYZ fin_pt = cloud->points[cloud_idx];
      
      tmp_pt = PclProcessing::pcl2vcgPt(fin_pt);    
      out_pts.push_back(tmp_pt);
    }
  } 

  DrawProgressBar(40, 1);
  std::cout<<"\n";
  return out_pts;
}

/**
   Function projects 2D change mask into 3-dimensional space using triangulation.
*/
cv::Mat ImgIO::projChngMaskTo3D(const cv::Mat &chngMask, const vcg::Shot<float> &cam1, const vcg::Shot<float> &cam2, const cv::Mat &H){
  
  std::vector<cv::Point2f> cam1_points, cam2_points;

  getPtsFromMask(chngMask, cam1_points);
  
  cv::Mat cam1_fmat;
  cv::Mat cam2_fmat;
 
  cv::Mat cam1_Rt = getRtMatrix(cam1);
  cv::Mat cam2_Rt = getRtMatrix(cam2);
  
  cv::Mat cam1_intr = getIntrMatrix(cam1);
  cv::Mat cam2_intr = getIntrMatrix(cam2);

  cam1_fmat = cam1_intr*cam1_Rt;
  cam2_fmat = cam2_intr*cam2_Rt;

  cv::perspectiveTransform(cam1_points, cam2_points, H);  

  cv::Mat pnts3D(1, cam1_points.size(), CV_64FC4);

  cv::triangulatePoints(cam1_fmat, cam2_fmat, cam1_points, cam2_points, pnts3D); 
  
  return pnts3D;
}

/**
   Class responsible for Input/Output operations.
*/
ChangeDetectorIO::ChangeDetectorIO(std::vector<std::string> inVector){
  filenames.resize(inVector.size());
  filenames = inVector;
}

ChangeDetectorIO::ChangeDetectorIO(std::string inDir){
  filenames.push_back(inDir);
}
/**
   Function saves frames from the input video for given frequency(frameRate).
*/
void VidIO::saveImgFromVideo(std::string outDir, int frameRate){
  
  cv::VideoCapture vidCap(filenames[0]);
  cv::Mat tmpImage;
  
  std::cout<<"Saving frames from video file..."<<std::endl;
  if(vidCap.isOpened()){
    for(int i = 0;;i++){
      ostringstream ss;
      ss<<i;
      vidCap>>tmpImage;

      if(i%frameRate==0){
	if(tmpImage.empty())
	  break;
	cv::imwrite(outDir+ss.str()+".jpg",tmpImage);
      }
    }
  }
  std::cout<<"Done!"<<std::endl;
}
/**
Function used to call external VisualSfM software
*/

void CmdIO::callVsfm(std::string inCmd){
  
  std::string outCommand;
  std::string vsfmCommand("VisualSFM");
  outCommand = vsfmCommand+inCmd;
  
  system(outCommand.c_str());
}
/**
Function to call the command line commands
*/
void CmdIO::callCmd(std::string inCmd){
  system(inCmd.c_str());
}

/**
Function reads lines from the input text file
*/
void FileIO::readNewFiles(const std::string &list_filename, std::vector<std::string> &out_filenames){

  std::ifstream inFile(list_filename.c_str());
  std::string tmpString;

  while(getline(inFile,tmpString))
    out_filenames.push_back(tmpString);

  inFile.close();
}

/**
   Function converts cameras read from NVM file into VCG Shot objects. Part of the function is based on the function Open() in import_out.h in VCG library
*/
std::vector<vcg::Shot<float> > FileIO::nvmCam2vcgShot(const std::vector<CameraT> &camera_data, const std::vector<std::string> names){
  
  std::cout<<"Converting NVM Cam structure to VCG shot structure..."<<std::endl;

  std::vector<vcg::Shot<float> > outputShots;  
  std::size_t inSize = camera_data.size();
  vcg::Shot<float> tmpShot;
  outputShots.resize(inSize);
  
  float f;
  float R[16] = {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1};
  vcg::Point3f t;
  CameraT tmpCam;
  int count;

  for(int i = 0 ; i < inSize ; i++){
    count = 0;

    tmpCam = camera_data[i];
    f = tmpCam.f;

    for(int j = 0; j < 3 ; j++){
      R[count] = tmpCam.m[j][0];
      R[count+1] = tmpCam.m[j][1];
      R[count+2] = tmpCam.m[j][2];
      count+=4;
    }
    
    t[0] = tmpCam.t[0];
    t[1] = tmpCam.t[1];
    t[2] = tmpCam.t[2];

    vcg::Matrix44f mat = vcg::Matrix44<vcg::Shotf::ScalarType>::Construct<float>(R);

    vcg::Matrix33f Rt = vcg::Matrix33f( vcg::Matrix44f(mat), 3);
    Rt.Transpose();

    vcg::Point3f pos = Rt * vcg::Point3f(t[0], t[1], t[2]);

    outputShots[i].Extrinsics.SetTra(vcg::Point3<vcg::Shotf::ScalarType>::Construct<float>(-pos[0],-pos[1],-pos[2]));
    outputShots[i].Extrinsics.SetRot(mat);
    outputShots[i].Intrinsics.FocalMm = f;
    outputShots[i].Intrinsics.k[0] = 0.0;
    outputShots[i].Intrinsics.k[1] = 0.0;
    outputShots[i].Intrinsics.PixelSizeMm = vcg::Point2f(1,1);
    
    cv::Mat image;
    image = cv::imread(names[i]);
    cv::Size size = image.size();

    outputShots[i].Intrinsics.ViewportPx = vcg::Point2i(size.width,size.height);
    outputShots[i].Intrinsics.CenterPx[0] = (int)((double)outputShots[i].Intrinsics.ViewportPx[0]/2.0f);
    outputShots[i].Intrinsics.CenterPx[1] = (int)((double)outputShots[i].Intrinsics.ViewportPx[1]/2.0f);
  }
  
  std::cout<<"Done."<<std::endl;
  return outputShots;
}

/**
Function loads data from NVM file
*/
std::map<std::string, int> FileIO::getNVM(std::string filename, std::vector<CameraT>& camera_data, std::vector<std::string>& names, std::vector<PtCamCorr>& pt_cam_corr, std::map<int, std::vector<ImgFeature> >& in_map){
  
  std::map<std::string,int> out_map;

  std::ifstream inFile(filename.c_str());
  
  std::cout<<"Loading NVM file... ";
  if(LoadNVM(inFile, camera_data, names, pt_cam_corr, in_map, out_map))
    std::cout<<"Done!"<<endl;
  inFile.close();
  return out_map;
}

/**
Function to ensure that NVM file has only one model
*/
bool FileIO::forceNVMsingleModel(std::ifstream& in, const std::string &nvm_name){
  
  std::ofstream tmp_os;
  std::string token;
  std::string tmp_name("tmp_os.nvm");
  tmp_os.open(tmp_name.c_str());
  
  if(in.peek() == 'N') 
    {
      in >> token; 
      tmp_os<<token<<"\n\n";
    }
  
  int ncam = 0, npoint = 0, nproj = 0;   
  in >> ncam;  if(ncam <= 1) return false; 
  tmp_os<<ncam;
  for(int i = 0; i < ncam+1; ++i)
    {
      getline(in, token);
      tmp_os<<token<<"\n";
    }
  tmp_os<<"\n";
  in >> npoint;

  tmp_os<<npoint;
  if(npoint <= 0)
    {
      std::cout << ncam << " new cameras\n";
      return true; 
    }
  
  for(int i = 0; i < npoint+1; ++i)
    {
      getline(in, token);
      tmp_os<<token<<"\n";
    }
  tmp_os<<"\n0\n";
  tmp_os<<"1 0\n";
  tmp_os.close();

  CmdIO::callCmd("cp "+tmp_name+" "+nvm_name);
  return true;
}

/**
Function reads K nearest neighbors for new images using feature matches stored in txt file
 */

void FileIO::getNewImgNN(const std::vector<std::string>& new_image_files, std::vector<std::vector<std::string> > &output, const std::string& matches_file, int K, std::vector<std::vector<std::vector<std::pair<int,int> > > > &feat_pairs){
  
  std::ifstream in_file(matches_file.c_str());

  std::cout<<"Finding nearest neighbors for new cameras... ";
  
  std::string tmp_string;
  std::map<std::string,int> map_value;
  std::map<std::string, std::string> n_map;
  std::map<std::string, int> idx_map;

  output.resize(new_image_files.size());
  feat_pairs.resize(new_image_files.size());
  
  //Create a map that for each new image containing integer value(highest number of matches) and a map with image indeces
  for(int i = 0 ; i < new_image_files.size(); i++){
    map_value[new_image_files[i]] = 0;
    idx_map[new_image_files[i]] = i;
    output[i].resize(K);
    feat_pairs[i].resize(K);
  }

  //Iterate through file
  while(getline(in_file, tmp_string)){
    
    //Check if it's a directory
    if(tmp_string[0]!='/')
      continue;
    
    std::string first_file = tmp_string;
    std::string second_file;

    getline(in_file, second_file);
    int no_of_matches = 0;
    in_file>>no_of_matches;
    
    //Find the file directory in the map
    std::map<std::string,int>::iterator tmp_itr = map_value.find(second_file);

    //Check if if the file is from new image set
    if(tmp_itr!=map_value.end())

      //Check if the nearest neighbor is not and image from the new set
      if(map_value.find(first_file)==map_value.end())	

	//Check if the number of matches is greater than the current one
	if(tmp_itr->second <= no_of_matches){	  	 
	  tmp_itr->second = no_of_matches;
	  n_map[second_file] = first_file;
	  
	  //Insert old file name at the beginning so in the result we get sorted vector of neighbors depending on number of matches
	  int idx = idx_map[second_file];
	  output[idx].insert(output[idx].begin(), first_file);

	  //Get feature pairs
	  std::vector<std::pair<int,int> > tmp_pairs(no_of_matches);
	  
	  for(int i = 0; i < no_of_matches; ++i)
	    in_file>>tmp_pairs[i].first;
	  for(int i = 0; i < no_of_matches; ++i)
	    in_file>>tmp_pairs[i].second;
	  
	  feat_pairs[idx].insert(feat_pairs[idx].begin(), tmp_pairs);
	}     
  }
  in_file.close();
}

void dispProjPt(const vcg::Point2i &inPt, cv::Mat &inImg){
  
  static cv::Scalar color = cv::Scalar(0, 0, 0);	
    
  cv::circle(inImg, cv::Point(inPt.X(),inPt.Y()), 50 , color, 15);
  
  cv::namedWindow( "Display window", cv::WINDOW_NORMAL );// Create a window for display.
  cv::imshow( "Display window", inImg);                   // Show our image inside it.
      
  cv::waitKey(0);                        
}

void getImgSet(std::vector<std::string> fileDirs, std::vector<cv::Mat> &outImgSet){

  std::cout<<"Loading images..."<<std::endl;
  cv::Mat image;

  for(std::vector<std::string>::iterator it = fileDirs.begin(); it!=fileDirs.end(); ++it){
    image = cv::imread("/home/bheliom/Pictures/NotreDame/"+*it, CV_LOAD_IMAGE_COLOR);
    std::cout<<"/home/bheliom/Pictures/NotreDame/"+*it<<std::endl;

    if(!image.data)
      std::cout<<"Could not open the file!"<<std::endl;
    else
      outImgSet.push_back(image);
  }

  std::cout<<"Done."<<std::endl;
}

cv::Mat getImg(std::string fileDirs){
  
  return cv::imread(fileDirs.c_str());//, CV_LOAD_IMAGE_COLOR);
}

void readCmdInput(std::map<int,std::string> &inStrings, int argc, char** argv){
  
  int flags, opt;
  int nsecs, tfnd;
  
  nsecs = 0;
  tfnd = 0;
  flags = 0;
  
  while ((opt = getopt(argc, argv, "m:p:b:i:o:n")) != -1) {
    switch (opt) {
	
    case 'm':
      inStrings[MESH] = optarg;
      break;
    case 'p':
      inStrings[PMVS] = optarg;
      break;
    case 'b':
      inStrings[BUNDLER] = optarg;
      break;
    case 'i':
      inStrings[IMAGELIST] = optarg;
      break;
    case 'o':
      inStrings[OUTDIR] = optarg;
      break;
	
    default: /* '?' */
      fprintf(stderr, "Usage: %s [-m input mesh] [-p input PMVS] [-b input bundler file] [-i input image list]\n",
	      argv[0]);
    }
  }
}

void getPlyFileVcg(std::string filename, MyMesh &m){

  vcg::tri::io::ImporterPLY<MyMesh> importVar;
  
  if(importVar.Open(m,filename.c_str()))
    {
      printf("Error reading file  %s\n",filename.c_str());
      std::cout<<vcg::tri::io::ImporterOFF<MyMesh>::ErrorMsg(importVar.Open(m,filename.c_str()))<<std::endl;
      exit(0);
    }
  std::cout<<"Mesh loaded correctly. No. of faces:"<<m.FN()<<" no. of vertices:"<<m.VN()<<std::endl;
}

void savePlyFileVcg(std::string filename, MyMesh &m){
  
  vcg::tri::io::ExporterPLY<MyMesh> exportVar;

  exportVar.Save(m,filename.c_str(),vcg::tri::io::Mask::IOM_VERTCOLOR);

}

void getBundlerFile(MyMesh &m, std::string filename, std::string filename_images, std::vector<vcg::Shot<float> > &shots, std::vector<std::string> &image_filenames){

  std::cout<<"Start reading bundler file..."<<std::endl;
  vcg::tri::io::ImporterOUT<MyMesh> importVar;

  if(importVar.Open(m, shots , image_filenames, filename.c_str(), filename_images.c_str()))
    std::cout<<"Error reading the bundler file!"<<std::endl;

  std::cout<<"Done."<<std::endl;
}
