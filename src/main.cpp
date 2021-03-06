//============================================================================
// Name        : Processor.cpp
// Author      : Marcelo E. Kaihara
// Version     :
// Copyright   : 
// Description : Hello World in C++, Ansi-style
//============================================================================

#include <iostream>
#include <thread>
#include <mutex>
#include <sstream>
#include <iostream>
#include <fstream>
#include <iterator>
#include <vector>
#include <opencv4/opencv2/core/types.hpp>
#include <opencv4/opencv2/core.hpp>
#include <opencv4/opencv2/features2d.hpp>
#include <opencv4/opencv2/imgcodecs.hpp>
#include <opencv4/opencv2/opencv.hpp>
#include <math.h>
#include <experimental/filesystem>

#include "Estimation.hpp"
#include "pipelineAlgo.hpp"
#include "Cartesian2Spherical.hpp"
#include "config.hpp"
#include "utils.hpp"
#include "log.hpp"
#include "Queue.hpp"
#include "QueueValue.hpp"
#include "ThreadPool.h"

using namespace std;
using namespace cv;
namespace fs = std::experimental::filesystem;

// Thread pool for processing the feature extraction
const int numberThreadsFeatureExtraction { 4 };
ThreadPool pool_features(numberThreadsFeatureExtraction);

// Thread-safe queue for enqueuing the processes for feature extraction
ScanVan::thread_safe_queue_future<std::future<std::shared_ptr<EquirectangularWithFeatures>>> featExtProcQueue {};

// Thread-safe queue for communication between the generation of the images and the feature extraction
ScanVan::thread_safe_queue<Equirectangular> imgProcQueue {};

// Thread-safe queue for communication between the feature extraction and processing of features
ScanVan::thread_safe_queue<EquirectangularWithFeatures> featureProcQueue {};

// Thread-safe queue for communication between the feature extraction and pose estimation
ScanVan::thread_safe_queue<TripletsWithMatches> tripletsProcQueue {};

// Thread-safe queue for communication sending the models for fusion
ScanVan::thread_safe_queue<Model> modelQueue {};


//=========================================================================================================

void generatePairImages (Log *mt, Config *FC) {
// It reads the images from file and pushes to the queue for the feature extraction

	// list of file names of the input images
	std::vector<std::string> file_list{};

	// read the contents of the directory where the images are located
	fs::path pt = fs::u8path(FC->inputFolder);
	for (auto& p : fs::directory_iterator(pt)) {
		std::string str = p.path().u8string();
		if (str.substr(str.length()-3)=="bmp") {
			file_list.push_back(p.path().u8string());
		}
	}

	// sort the filenames alphabetically
	std::sort(file_list.begin(), file_list.end());

//	auto it1 = std::find(file_list.begin(),file_list.end(),"data_in/0_dataset/20181218-161153-843291.bmp");


	/*auto it = std::find(file_list.begin(),file_list.end(),inputFolder + "/" + inputDataSet + "/" + "20181218-161515-093294.bmp");
	file_list.erase(file_list.begin(), it);

	it = std::find(file_list.begin(),file_list.end(),inputFolder + "/" + inputDataSet + "/" + "20181218-162021-343307.bmp");
	file_list.erase(it, file_list.end());
*/

//	auto it2 = std::find(file_list.begin(),file_list.end(),"data_in/0_dataset/20181218-161314-343305.bmp");
//	file_list.erase(it, file_list.end());
	for (auto &n: file_list) {
		std::cout << n << std::endl;
	}
	std::cout << "Number of files considered: " << file_list.size() << std::endl;


	/*auto it1 = std::find(file_list.begin(), file_list.end(), "/media/mkaihara/SCANVAN10TB/record/camera_40008603-40009302/20190319-103441_SionCar1/20190319-104004-844894.bmp");
	file_list.erase(file_list.begin(), it1);

	for (auto &n : file_list) {
		std::cout << n << std::endl;
	}
	std::cout << "Number of files considered: " << file_list.size() << std::endl;*/


/*	auto it2 = std::find(file_list.begin(), file_list.end(), "data_in/0_dataset/20181218-161530-093291.bmp");
	file_list.erase(it2, file_list.end());
	std::cout << "Number of files considered: " << file_list.size() << std::endl;
*/

	//file_list.erase(file_list.begin(),file_list.begin()+3);
	//file_list.erase(file_list.begin()+3, file_list.end());

	// counter for the image number
	int img_counter { 1 };

	for (auto &file: file_list) {

		// reads the image from the file
		cv::Mat input_image { };
		input_image = imread(file, cv::IMREAD_UNCHANGED);
		if (!input_image.data) {
			throw std::runtime_error("Could not load the input image");
		}

		// removes the folder name that precedes the path
		std::string fileName = file.substr(file.find_last_of("/", std::string::npos) + 1, std::string::npos);
		//std::cout << fileName << '\n';

		// creates a shared pointer from an anonymous object initialized with the image
		std::shared_ptr<Equirectangular> p1(new Equirectangular { input_image, img_counter, fileName });

		// simulates the delay of image acquisition
		//std::this_thread::sleep_for(std::chrono::milliseconds(1000));

		while (imgProcQueue.size()>2) {
			std::this_thread::sleep_for(1s);
		}

		// push to the queue
		imgProcQueue.push(p1);

		std::stringstream ss { };
		ss << "=========================" << std::endl
		   << "Send pair images " << p1->getImgNum() << std::endl
		   << "========================="
		   << std::endl;
		print(ss.str());

		img_counter++;
	}

	// signals the end of genPairs
	mt->terminateGenPairs = true;
	std::cout << "==> generatePairImages finished." << std::endl;
}

//=========================================================================================================

void extractFeaturesImages (Log *mt, Config *FC) {

	// reads the mask to apply on the images
	//auto mask = make_shared<cv::Mat>(imread(inputFolder + "/" + inputMask + "/" + inputMaskFileName, IMREAD_GRAYSCALE));
	auto mask = make_shared<cv::Mat>(imread(FC->inputMask, IMREAD_GRAYSCALE));

	// If the mask was not loaded, throw an error
	if (! mask->data) {
		throw std::runtime_error("The image mask could not be read.");
	}

	// loop over while not terminate or the queue is not empty
	while ((!mt->terminateGenPairs)||(!imgProcQueue.empty())) {

		std::shared_ptr<Equirectangular> receivedPairImages { };
		receivedPairImages = imgProcQueue.wait_pop();

		std::stringstream ss {};
		ss << "=========================" << std::endl
		   << "Received equirectangular image " << receivedPairImages->getImgNum() << std::endl
		   << "=========================" << std::endl;
		print (ss.str());

		while (featExtProcQueue.size() > numberThreadsFeatureExtraction) {
			// Reached the number of images for parallel processing
			std::this_thread::sleep_for(1s);
		}

		//mt->start("1. Feature Extraction"); // measures the feature extraction
		////////////////////////////////////////////////////////////////////////////////////////////////////////
		auto m = pool_features.enqueue([receivedPairImages, mask] {
			return extractFeatures(receivedPairImages, mask);
		});
		featExtProcQueue.push(m);
		////////////////////////////////////////////////////////////////////////////////////////////////////////
		//mt->stop("1. Feature Extraction");  // measures the feature extraction

	}

	// signals the end of genPairs
	mt->terminateFeatureExtraction = true;
	std::cout << "==> Feature extraction finished." << std::endl;

}


void procFeatures (Log *mt, Config *FC) {

	long int tripletSeqNum { 0 };

	std::deque<std::shared_ptr<EquirectangularWithFeatures>> v { };
	std::deque<std::shared_ptr<PairWithMatches>> lp { };

	// loop over while not terminate or the queue is not empty
	while ((!mt->terminateFeatureExtraction)||(!featExtProcQueue.empty())) {

		//std::future<std::shared_ptr<EquirectangularWithFeatures>> rcv_result {};
		auto rcvResult = featExtProcQueue.wait_pop();

		std::shared_ptr<EquirectangularWithFeatures> receivedImgWithFeatures { };
		receivedImgWithFeatures = rcvResult.get();

		//==========================================================================================
		// write into file the features
		FC->write_1_features(receivedImgWithFeatures);
		//==========================================================================================

		// v is a sort of queue where the extracted features are stored
		v.push_front(receivedImgWithFeatures);
		// whenever two sets of features are extracted, the matches between these sets are pushed to lp
		// and the last set of features is discarded
		if (v.size() == 2) {

			mt->start("2. Feature Matching Pairs"); // measures the feature matching of pairs
			////////////////////////////////////////////////////////////////////////////////////////////////////////
			lp.push_front(omniMatching(v[1], v[0]));
			////////////////////////////////////////////////////////////////////////////////////////////////////////
			mt->stop("2. Feature Matching Pairs"); // measures the feature matching of pairs

			//==========================================================================================
			// write the matched features for each pair of images
			FC->write_2_matches(lp.front());
			//==========================================================================================

			//==========================================================================================
			// Check here if car is moving
			//==========================================================================================
			if (!movementCheck(lp.front(), FC->stillThrs)) {
			// If it is not in movement, remove from the deque
				lp.pop_front();
				v.pop_front();
			} else {
				// If it is in movement, write the features and continue the computation

				//==========================================================================================
				// write the matched features for each pair of images
				FC->write_2_matches_moving (lp.front());
				//==========================================================================================

				//==========================================================================================
				// write the matched features for each pair of images
				FC->write_2_matches_moving_index(lp.front());
				//==========================================================================================

				v.pop_back();

			}

		}

		// If the execution type is FILTER_STILL skip the rest of the computation
		if (FC->execType == Config::FILTER_STILL) {
			lp.clear();
			continue;
		}
		// lp is a sort of queue where the matches are stored
		// whenever there are two sets of matches, the triplets are computed
		if (lp.size() == 2) {

			// calculates the keypoints common on two pairs of images
			// in this case it creates a triplet

			mt->start("3. Common Points Computation - Triplets"); // measures the common points computation
			////////////////////////////////////////////////////////////////////////////////////////////////////////
			std::shared_ptr<TripletsWithMatches> p1 = commonPointsComputation(lp[1], lp[0]);
			////////////////////////////////////////////////////////////////////////////////////////////////////////
			mt->stop("3. Common Points Computation - Triplets"); // measures the common points computation

			tripletSeqNum++;
			p1->setTripletSeqNum(tripletSeqNum);

			// push the triplet to the thread-safe queue and send it for pose estimation
			while (tripletsProcQueue.size() > 2) {
				std::this_thread::sleep_for(1s);
			}

			tripletsProcQueue.push(p1);

			//==========================================================================================
			// write the matched features for two consecutive pair of images, i.e. triplets
			FC->write_3_triplets(p1);
			//==========================================================================================

			lp.pop_back();

		}
	}


	// Queue empty objects to signal end of processing
	std::shared_ptr<TripletsWithMatches> p1(new TripletsWithMatches { });
	p1->setTripletSeqNum(-1);
	tripletsProcQueue.push(p1);
	std::shared_ptr<TripletsWithMatches> p2(new TripletsWithMatches { });
	p2->setTripletSeqNum(-1);
	tripletsProcQueue.push(p2);
	std::shared_ptr<TripletsWithMatches> p3(new TripletsWithMatches { });
	p3->setTripletSeqNum(-1);
	tripletsProcQueue.push(p3);

	std::cout << "==> procFeatures finished." << std::endl;
	// signals the end of procFeatures
	mt->terminateProcFeatures = true;

}

//========================================================================================================

void ProcPose (Log *mt, Config *FC) {

	RNG rng(12345);

	std::thread::id this_id = std::this_thread::get_id();
	std::stringstream ss_thread {};
	ss_thread << this_id;


	while((!mt->terminateProcFeatures)||(!tripletsProcQueue.empty())) {

		// gets the triplets from procFeatures
		std::shared_ptr<TripletsWithMatches> receivedTripletsImages { };
		receivedTripletsImages = tripletsProcQueue.wait_pop();

		// If received a end of processing exit the while loop
		if (receivedTripletsImages->getTripletSeqNum() == -1) {
			break;
		}

		// print message
		std::stringstream ss { };
		ss << "=========================" << std::endl
		   << "Received triplets images " << "(" << receivedTripletsImages->getImageNumber1() << ", " << receivedTripletsImages->getImageNumber2() << ", " << receivedTripletsImages->getImageNumber3() << ")" << std::endl
   		   << "=========================" << std::endl;
		print(ss.str());

		// vector containing the spherical coordinates
		std::vector<Vec_Points<double>> p3d_liste { };


		// width and height of the images
		auto width = receivedTripletsImages->getImage()[0]->getOmni()->getImage().cols;
		auto height = receivedTripletsImages->getImage()[0]->getOmni()->getImage().rows;

		mt->start("4. Conversion to Spherical - Triplets " + ss_thread.str()); // measures the conversion time to spherical
		// loop over the spheres
		for (int idx { 0 }; idx < 3; ++idx) {

			Vec_Points<double> list_matches { };

			// if there are matches then convert them to spherical
			if (receivedTripletsImages->getMatchVector().size() != 0) {
				for (const auto &match : receivedTripletsImages->getMatchVector()) {
					// match is a vector of indices of the keypoints
					// for triplets its size is 3

					// gets the xy coordinate of the keypoint corresponding to sphere idx
					auto keypoint = receivedTripletsImages->getImage()[idx]->getKeyPoints()[match[idx]].pt;

					// convert x and y coordinates to spherical
					Points<double> p = convertCartesian2Spherical(static_cast<double>(keypoint.x), static_cast<double>(keypoint.y), width, height);

					// push_back the spherical coordinate into list_matches
					list_matches.push_back(p);
				}
			}

			p3d_liste.push_back(list_matches);
		}
		mt->stop("4. Conversion to Spherical - Triplets " + ss_thread.str()); // measures the conversion time to spherical

		// copy of the original vector containing the spherical coordinates
		std::vector<Vec_Points<double>> p3d_liste_orig {p3d_liste};

		//==========================================================================================
		// output in a file the spherical coordinates of the triplet
		FC->write_4_spherical(receivedTripletsImages, p3d_liste);
		//==========================================================================================

		Vec_Points<double> sv_scene { };
		std::vector<Points<double>> positions { Points<double>(), Points<double>(), Points<double>() };
		std::vector<Points<double>> sv_t_liste { Points<double>(), Points<double>()};
		std::vector<Mat_33<double>> sv_r_liste { Mat_33<double>(), Mat_33<double>()};

		double error_max { 1e-8 };

		//-----------------------------------------------------
		// call to pose estimation algorithm
		int initialNumberFeatures = p3d_liste[0].size();

		int numIter {};

		// Only compute if there are features in the vector
		if (initialNumberFeatures!=0) {
			mt->start("5. Pose Estimation " + ss_thread.str()); // measures the time of pose estimation algorithm

			for (int i = 0; i < 1; ++i) {
				numIter = pose_estimation(p3d_liste, error_max, sv_scene, positions, sv_r_liste, sv_t_liste);

				//filter_keypoints(p3d_liste, sv_scene, positions, p3d_liste);

				std::cout << "Number of iterations pose estimation : " << numIter << std::endl;
			}

			mt->stop("5. Pose Estimation " + ss_thread.str()); // measures the time of pose estimation algorithm
		}

		int finalNumberFeatures = p3d_liste[0].size();
		//-----------------------------------------------------

		//==========================================================================================
		// output in a file the rotation matrix and translation vector and statistics of the pose estimation algorithm
		FC->write_5_pose_3(receivedTripletsImages, sv_r_liste, sv_t_liste, numIter, initialNumberFeatures, finalNumberFeatures);
		//==========================================================================================

		//==========================================================================================
		// output in a file of the sparse point cloud of the triplet
		FC->write_6_sparse_3 (receivedTripletsImages, sv_scene);
		//==========================================================================================

		//==========================================================================================
		// write the matched features of the triplets that are filtered
		FC->write_3_triplets_filtered (receivedTripletsImages, p3d_liste_orig, p3d_liste);
		//==========================================================================================


		// The new model to add
		std::shared_ptr<Model> m2(new Model {});
		//Model m2 { };

		// put the names of the images into the model
		m2->imgNames.push_back(receivedTripletsImages->getImageName1());
		m2->imgNames.push_back(receivedTripletsImages->getImageName2());
		m2->imgNames.push_back(receivedTripletsImages->getImageName3());
		m2->modelSeqNum = receivedTripletsImages->getTripletSeqNum();

		// the position of the center of the triplets
		cv::Matx13f modelCenter(0, 0, 0);

		// calculates the mean position of the triplets
		for (size_t i { 0 }; i < positions.size(); ++i) {
			Points<double> f = positions[i];
			modelCenter = modelCenter + Matx13f(f[0], f[1], f[2]);
		}
		modelCenter = 1.0 / positions.size() * modelCenter;

		// calculates the average distance of the reconstructed points with respect to the center of the triplets
		double averageDistance = 0;
		for (size_t i { 0 }; i < sv_scene.size(); ++i) {
			Points<double> f = sv_scene[i];
			averageDistance += norm(modelCenter - Matx13f(f[0], f[1], f[2]));
		}
		averageDistance /= sv_scene.size();

		// assigns a random color to the model to add
		RGB888 modelColor = RGB888(rng.uniform(0, 255), rng.uniform(0, 255), rng.uniform(0, 255));

		// copies to features vector of the model the points whose average distance is relatively close
		for (size_t i { 0 }; i < sv_scene.size(); ++i) {
			Points<double> f = sv_scene[i];
			/*if (norm(Matx13f(f[0], f[1], f[2]) - modelCenter) > averageDistance)
				continue;*/
			//m2.features.push_back(ModelFeature(1000 * Matx13f(f[0], f[1], f[2]), modelColor));  //RGB888(255,255, counter * 0x40)
			m2->features.push_back(ModelFeature(Matx13f(f[0], f[1], f[2]), modelColor));  //RGB888(255,255, counter * 0x40)
		}

		// copies to camera positions vector the positions of the spheres
		for (size_t i { 0 }; i < positions.size(); ++i) {
			//Points<double> f = positions[i];
			// m2, i.e. the new model of the triplet contains the positions of the camera where the first position is (0,0,0)
			// and the rotation matrices, the first rotation matrix is eye(3,3), the second sv_r_liste[0], i.e. r12, and the third sv_r_liste[1], i.e. r23
			if (i==0) {
				m2->viewPoints.push_back(ModelViewPoint(cv::Matx13f::zeros(), cv::Matx33f::eye(), cv::Matx33f::eye()));
			} else if (i > 0) {
				Mat_33<double> & m1 = sv_r_liste[i - 1];
				Points<double> f = positions[i];
				// the relative rotation of the camera position with respect to the previous camera position
				cv::Matx33d mat1 { m1[0][0], m1[0][1], m1[0][2], m1[1][0], m1[1][1], m1[1][2], m1[2][0], m1[2][1], m1[2][2] };
				// the absolute rotation of the camera position with respect to the first camera position of the triplet
				cv::Matx33d mat2 = (m2->viewPoints[i-1].rotationAbsolute) * mat1.t();
				m2->viewPoints.push_back(ModelViewPoint(cv::Matx13d(f[0], f[1], f[2]), mat1, mat2));
			}
		}

		modelQueue.push(m2);
	}

	// signals the end of pose estimation
	mt->terminateProcPose--;

	// Send a model with sequence number -1 to terminate the process
	std::shared_ptr<Model> m(new Model { });
	m->modelSeqNum = -1;
	modelQueue.push(m);

	std::cout << "==> ProcPose finished." << std::endl;
}


void FusionModel (Log *mt, Config *FC) {

	// the main model where all the models will be superposed
	Model m { };
	m.modelSeqNum = 0;
	std::deque<std::shared_ptr<Model>> vecModel { };

	while((mt->terminateProcPose)||(!modelQueue.empty())) {

		// gets the triplets from procFeatures
		std::shared_ptr<Model> receivedModel { };
		receivedModel = modelQueue.wait_pop();

		if (receivedModel->modelSeqNum == -1) {
			continue;
		}

		// print message
		std::stringstream ss { };
		ss << "=========================" << std::endl
		   << "Received model" << "(" << receivedModel->modelSeqNum << ")" << std::endl
		   << "=========================" << std::endl;
		print(ss.str());

		vecModel.push_back(receivedModel);
		std::sort(vecModel.begin(), vecModel.end(), [](std::shared_ptr<Model> x, std::shared_ptr<Model> y) { return x->modelSeqNum < y->modelSeqNum; } );

		bool continue2check { true };

		while (!vecModel.empty() && continue2check) {
			if (vecModel[0]->modelSeqNum == (m.modelSeqNum + 1)) {

				mt->start("6. Fusion"); // measures the time of fusion algorithm
				//-----------------------------------------------------
				fusionModel2(&m, &(*vecModel[0]), 2);
				//-----------------------------------------------------
				mt->stop("6. Fusion"); // measures the time of fusion algorithm

				m.modelSeqNum++;
				vecModel.pop_front();

				//==========================================================================================
				// output in a file of the absolute rotation and translation matrices
				FC->write_7_odometry(m);
				//==========================================================================================

				//==========================================================================================
				// output in a file of the progressively merged models
				FC->write_8_progressiveModel(m);
				//==========================================================================================
			} else {
				continue2check = false;
			}
		}
	}

	std::cout << "==> FusionModel finished." << std::endl;
	//==========================================================================================
	// output in a file of the merged model
	FC->write_9_finalModel(m);
	//==========================================================================================

}

void RunAllPipeline (Config *FC) {

	Log mt{};

	mt.start("7. Total running time"); // measures the total running time

	// check if folders for writing the results exist
	FC->CheckFolders();

	std::thread GenPairs (generatePairImages, &mt, FC);
	std::thread ExtractFeatures (extractFeaturesImages, &mt, FC);
	std::thread ProcessFeatureExtraction (procFeatures, &mt, FC);

	mt.terminateProcPose = 3;
	std::thread ProcessPoseEstimation_1 (ProcPose, &mt, FC);
	std::thread ProcessPoseEstimation_2 (ProcPose, &mt, FC);
	std::thread ProcessPoseEstimation_3 (ProcPose, &mt, FC);
	std::thread ProcessFusion (FusionModel, &mt, FC);

	GenPairs.join();
	ExtractFeatures.join();
	ProcessFeatureExtraction.join();
	ProcessPoseEstimation_1.join();
	ProcessPoseEstimation_2.join();
	ProcessPoseEstimation_3.join();
	ProcessFusion.join();

	mt.stop("7. Total running time"); // measures the total running time

	mt.listRunningTimes();

}

void RunUntilFilterStill (Config *FC) {

	Log mt{};

	mt.start("7. Total running time"); // measures the total running time

	// check if folders for writing the results exist
	FC->CheckFolders();

	std::thread GenPairs (generatePairImages, &mt, FC);
	std::thread ProcessFeatureExtraction (procFeatures, &mt, FC);

	GenPairs.join();
	ProcessFeatureExtraction.join();

	mt.stop("7. Total running time"); // measures the total running time

	mt.listRunningTimes();
}

int main(int argc, char* argv[]) {

	if (argc < 2) {
		std::cerr << "Usage: " << argv[0] << " config_file.txt" << std::endl;
		return 1;
	}

	std::string cfg = argv[1];

	Config FC (cfg);

	if (FC.execType == Config::RUN_ALL) {
		RunAllPipeline(&FC);
	} else if (FC.execType == Config::FILTER_STILL) {
		RunUntilFilterStill(&FC);
	}

	return 0;

}






