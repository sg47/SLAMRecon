/**
* This file is part of ORB-SLAM2.
*
* Copyright (C) 2014-2016 Ra��l Mur-Artal <raulmur at unizar dot es> (University of Zaragoza)
* For more information see <https://github.com/raulmur/ORB_SLAM2>
*
* ORB-SLAM2 is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* ORB-SLAM2 is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with ORB-SLAM2. If not, see <http://www.gnu.org/licenses/>.
*/

#include "LocalMapping.h"
#include "Optimizer.h"

namespace SLAMRecon {

	LocalMapping::LocalMapping(Map *pMap, KeyFrameDatabase* pDB, CovisibilityGraph* pCoGraph, SpanningTree* pSpanTree)
		:m_pMap(pMap), m_bAbortBA(false), m_bStopped(false), m_bStopRequested(false), m_bNotStop(false), m_bAcceptKeyFrames(true),
		m_bFinishRequested(false), m_bFinished(true), m_pCoGraph(pCoGraph), m_pSpanTree(pSpanTree), m_pKeyFrameDB(pDB)
	{
	}

	LocalMapping::~LocalMapping() {

	}

	void LocalMapping::SetLoopCloser(LoopClosing* pLoopCloser) {
		m_pLoopCloser = pLoopCloser;
	}

	void LocalMapping::SetTracker(Tracking *pTracker) {

		m_pTracker = pTracker;
	}


	void LocalMapping::Run() {

		m_bFinished = false;

		while (1) {

			// Tracking will see that Local Mapping is busy 
			SetAcceptKeyFrames(false);

			// Check if there are keyframes in the queue
			if (CheckNewKeyFrames()) {

				// BoW conversion and insertion in Map
				// A. KeyFrame Insertion
				ProcessNewKeyFrame();

				// Check recent MapPoints
				// B. Recent Map Points Culling
				MapPointCulling();

				// Triangulate new MapPoints
				// C. New Map Point Creation
				CreateNewMapPoints();
				 
				if (!CheckNewKeyFrames()) {
					// Find more matches in neighbor keyframes and fuse point duplications
					SearchInNeighbors();
				}

				m_bAbortBA = false;

				if (!CheckNewKeyFrames() && !stopRequested())
				{
					// Local BA
					// D. Local Bundle Adjustment
					if (m_pMap->KeyFramesInMap() > 2) {
						Optimizer::LocalBundleAdjustment(m_pCurrentKeyFrame, &m_bAbortBA, m_pMap, m_pCoGraph);
					}

					// Check redundant local Keyframes
					// E. Local KeyFrame Culling
					KeyFrameCulling();
				}
				 
				m_pLoopCloser->InsertKeyFrame(m_pCurrentKeyFrame);

				cout << "After LocalMapping, Current KeyFrame Id: " << m_pCurrentKeyFrame->m_nKFId << endl;
			} else if (Stop()) {

				// Safe area to stop
				while (isStopped() && !CheckFinish()) {
					Sleep(3);
				}
				if (CheckFinish())
					break;
			}

			ResetIfRequested();

			//// Tracking will see that Local Mapping is free
			SetAcceptKeyFrames(true);

			if (CheckFinish())
				break;

			Sleep(3);
		}

		SetFinish();
	}

	void LocalMapping::InsertKeyFrame(KeyFrame *pKF) {
		unique_lock<mutex> lock(m_MutexNewKFs);
		m_lpNewKeyFrames.push_back(pKF);
	}

	bool LocalMapping::CheckNewKeyFrames() {
		unique_lock<mutex> lock(m_MutexNewKFs);
		return(!m_lpNewKeyFrames.empty());
	}

	void LocalMapping::ProcessNewKeyFrame() {

		{
			unique_lock<mutex> lock(m_MutexNewKFs);
			m_pCurrentKeyFrame = m_lpNewKeyFrames.front();
			m_lpNewKeyFrames.pop_front();
			cout << "Before LocalMapping, Current KeyFrame Id: " << m_pCurrentKeyFrame->m_nKFId << endl;
		}
		 
		m_pCurrentKeyFrame->ComputeBoW();

		// Associate MapPoints to the new keyframe and update normal and descriptor
		const vector<MapPoint*> vpMapPointMatches = m_pCurrentKeyFrame->GetMapPointMatches();

		for (size_t i = 0; i < vpMapPointMatches.size(); i++) {
			MapPoint* pMP = vpMapPointMatches[i];
			if (pMP) {
				if (!pMP->isBad()) {
					 
					
					if (!pMP->IsInKeyFrame(m_pCurrentKeyFrame)) { 
						pMP->AddObservation(m_pCurrentKeyFrame, i);
						pMP->UpdateNormalAndDepth();
						pMP->ComputeDistinctiveDescriptors();
					} else { 
						m_lpRecentAddedMapPoints.push_back(pMP);
					}
				}
			}
		}

		// Update links in the Covisibility Graph
		m_pCoGraph->UpdateConnections(m_pCurrentKeyFrame);
		m_pSpanTree->UpdateConnections(m_pCurrentKeyFrame);

		// Insert Keyframe in Map
		m_pMap->AddKeyFrame(m_pCurrentKeyFrame);
		cout << "Add KeyFrame  " << m_pCurrentKeyFrame->m_nKFId << endl;
		 
	}

	void LocalMapping::MapPointCulling() {
		// Check Recent Added MapPoints
		list<MapPoint*>::iterator lit = m_lpRecentAddedMapPoints.begin();
		const unsigned long int nCurrentKFid = m_pCurrentKeyFrame->m_nKFId;

		const int cnThObs = 3;

		while (lit != m_lpRecentAddedMapPoints.end()) {
			MapPoint* pMP = *lit;
			if (pMP->isBad()) {
				lit = m_lpRecentAddedMapPoints.erase(lit);
 
			} else if (pMP->GetFoundRatio() < 0.25f) { 
				pMP->SetBadFlag();
				lit = m_lpRecentAddedMapPoints.erase(lit);
				 
			} else if (((int)nCurrentKFid - (int)pMP->m_nFirstKFid) >= 2 && pMP->Observations() <= cnThObs) {
				pMP->SetBadFlag();
				lit = m_lpRecentAddedMapPoints.erase(lit);
				 cout << "error 1" << endl; 
			} else if (((int)nCurrentKFid - (int)pMP->m_nFirstKFid) >= 3) {
				lit = m_lpRecentAddedMapPoints.erase(lit);
				 cout << "error 2" << endl;
			}
			else
				lit++;
		}
	}


	void LocalMapping::CreateNewMapPoints()
	{
		// Retrieve neighbor keyframes in covisibility graph
		int nn = 10;

		const vector<KeyFrame*> vpNeighKFs = m_pCoGraph->GetBestCovisibilityKeyFrames(m_pCurrentKeyFrame, nn);

		ORBmatcher matcher(0.6, false);

		// get camera paremeters
		cv::Mat Rcw1 = m_pCurrentKeyFrame->GetRotation();
		cv::Mat Rwc1 = Rcw1.t();
		cv::Mat tcw1 = m_pCurrentKeyFrame->GetTranslation();
		cv::Mat Tcw1(3, 4, CV_32F);
		Rcw1.copyTo(Tcw1.colRange(0, 3));
		tcw1.copyTo(Tcw1.col(3));
		cv::Mat Ow1 = m_pCurrentKeyFrame->GetCameraCenter();

		const float &fx1 = m_pCurrentKeyFrame->m_pCameraInfo->m_fx;
		const float &fy1 = m_pCurrentKeyFrame->m_pCameraInfo->m_fy;
		const float &cx1 = m_pCurrentKeyFrame->m_pCameraInfo->m_cx;
		const float &cy1 = m_pCurrentKeyFrame->m_pCameraInfo->m_cy;
		const float &invfx1 = m_pCurrentKeyFrame->m_pCameraInfo->m_invfx;
		const float &invfy1 = m_pCurrentKeyFrame->m_pCameraInfo->m_invfy;

		const float ratioFactor = 1.5f * m_pCurrentKeyFrame->m_pPLevelInfo->m_fScaleFactor; // ��������

		int nnew = 0;

		// Search matches with epipolar restriction and triangulate
		for (size_t i = 0; i<vpNeighKFs.size(); i++) { 
			if (i>0 && CheckNewKeyFrames())
				return;

			KeyFrame* pKF2 = vpNeighKFs[i];

			// Check first that baseline is not too short
			cv::Mat Ow2 = pKF2->GetCameraCenter();
			cv::Mat vBaseline = Ow2 - Ow1;
			const float baseline = cv::norm(vBaseline);


			if (baseline < pKF2->m_pCameraInfo->m_bf / pKF2->m_pCameraInfo->m_fx)  
				continue;
			
			// Compute Fundamental Matrix
			cv::Mat F12 = ComputeF12(m_pCurrentKeyFrame, pKF2);

			// Search matches that fullfil epipolar constraint 
			vector<pair<size_t, size_t> > vMatchedIndices;
			matcher.SearchForTriangulation(m_pCurrentKeyFrame, pKF2, F12, vMatchedIndices, false);

			cv::Mat Rcw2 = pKF2->GetRotation();
			cv::Mat Rwc2 = Rcw2.t();
			cv::Mat tcw2 = pKF2->GetTranslation();
			cv::Mat Tcw2(3, 4, CV_32F);
			Rcw2.copyTo(Tcw2.colRange(0, 3));
			tcw2.copyTo(Tcw2.col(3));

			const float &fx2 = pKF2->m_pCameraInfo->m_fx;
			const float &fy2 = pKF2->m_pCameraInfo->m_fy;
			const float &cx2 = pKF2->m_pCameraInfo->m_cx;
			const float &cy2 = pKF2->m_pCameraInfo->m_cy;
			const float &invfx2 = pKF2->m_pCameraInfo->m_invfx;
			const float &invfy2 = pKF2->m_pCameraInfo->m_invfy;

			// Triangulate each match
			const int nmatches = vMatchedIndices.size();
			for (int ikp = 0; ikp<nmatches; ikp++) {

				const int &idx1 = vMatchedIndices[ikp].first;
				const int &idx2 = vMatchedIndices[ikp].second;

				const cv::KeyPoint &kp1 = m_pCurrentKeyFrame->m_vKeysUn[idx1];
				const float kp1_ur = m_pCurrentKeyFrame->m_vuRight[idx1];
				bool bStereo1 = kp1_ur >= 0;

				const cv::KeyPoint &kp2 = pKF2->m_vKeysUn[idx2];
				const float kp2_ur = pKF2->m_vuRight[idx2];
				bool bStereo2 = kp2_ur >= 0;

				// Check parallax between rays
				cv::Mat xn1 = (cv::Mat_<float>(3, 1) << (kp1.pt.x - cx1)*invfx1, (kp1.pt.y - cy1)*invfy1, 1.0);
				cv::Mat xn2 = (cv::Mat_<float>(3, 1) << (kp2.pt.x - cx2)*invfx2, (kp2.pt.y - cy2)*invfy2, 1.0);

				cv::Mat ray1 = Rwc1*xn1;
				cv::Mat ray2 = Rwc2*xn2;
				const float cosParallaxRays = ray1.dot(ray2) / (cv::norm(ray1)*cv::norm(ray2));
				
				float cosParallaxStereo = cosParallaxRays + 1;
				float cosParallaxStereo1 = cosParallaxStereo;
				float cosParallaxStereo2 = cosParallaxStereo;

				if (bStereo1)
					cosParallaxStereo1 = cos(2 * atan2(m_pCurrentKeyFrame->m_pCameraInfo->m_bf / m_pCurrentKeyFrame->m_pCameraInfo->m_fx / 2, m_pCurrentKeyFrame->m_vfDepth[idx1]));
				else if (bStereo2)
					cosParallaxStereo2 = cos(2 * atan2(pKF2->m_pCameraInfo->m_bf / pKF2->m_pCameraInfo->m_fx / 2, pKF2->m_vfDepth[idx2]));

				cosParallaxStereo = min(cosParallaxStereo1, cosParallaxStereo2);

				cv::Mat x3D;
				if (cosParallaxRays > 0 &&  cosParallaxRays < 0.9998) {

					// Linear Triangulation Method
					cv::Mat A(4, 4, CV_32F);
					A.row(0) = xn1.at<float>(0)*Tcw1.row(2) - Tcw1.row(0);
					A.row(1) = xn1.at<float>(1)*Tcw1.row(2) - Tcw1.row(1);
					A.row(2) = xn2.at<float>(0)*Tcw2.row(2) - Tcw2.row(0);
					A.row(3) = xn2.at<float>(1)*Tcw2.row(2) - Tcw2.row(1);

					cv::Mat w, u, vt;
					cv::SVD::compute(A, w, u, vt, cv::SVD::MODIFY_A | cv::SVD::FULL_UV);

					x3D = vt.row(3).t();

					if (x3D.at<float>(3) == 0)
						continue;

					// Euclidean coordinates
					x3D = x3D.rowRange(0, 3) / x3D.at<float>(3);

				} else
					continue; //No stereo and very low parallax

				cv::Mat x3Dt = x3D.t();

				//Check triangulation in front of cameras
				float z1 = Rcw1.row(2).dot(x3Dt) + tcw1.at<float>(2);
				if (z1 <= 0)
					continue;

				float z2 = Rcw2.row(2).dot(x3Dt) + tcw2.at<float>(2);
				if (z2 <= 0)
					continue;

				//Check reprojection error in first keyframe
				const float &sigmaSquare1 = m_pCurrentKeyFrame->m_pPLevelInfo->m_vLevelSigma2[kp1.octave];
				const float x1 = Rcw1.row(0).dot(x3Dt) + tcw1.at<float>(0);
				const float y1 = Rcw1.row(1).dot(x3Dt) + tcw1.at<float>(1);
				const float invz1 = 1.0 / z1;

				
				float u1 = fx1*x1*invz1 + cx1;
				float v1 = fy1*y1*invz1 + cy1;
				float errX1 = u1 - kp1.pt.x;
				float errY1 = v1 - kp1.pt.y;
				if ((errX1*errX1 + errY1*errY1) > 5.991*sigmaSquare1)
					continue;
				
				//Check reprojection error in second keyframe
				const float sigmaSquare2 = pKF2->m_pPLevelInfo->m_vLevelSigma2[kp2.octave];
				const float x2 = Rcw2.row(0).dot(x3Dt) + tcw2.at<float>(0);
				const float y2 = Rcw2.row(1).dot(x3Dt) + tcw2.at<float>(1);
				const float invz2 = 1.0 / z2;
				
				float u2 = fx2*x2*invz2 + cx2;
				float v2 = fy2*y2*invz2 + cy2;
				float errX2 = u2 - kp2.pt.x;
				float errY2 = v2 - kp2.pt.y;
				if ((errX2*errX2 + errY2*errY2) > 5.991*sigmaSquare2)
					continue;
				

				//Check scale consistency
				cv::Mat normal1 = x3D - Ow1;
				float dist1 = cv::norm(normal1);

				cv::Mat normal2 = x3D - Ow2;
				float dist2 = cv::norm(normal2);

				if (dist1 == 0 || dist2 == 0)
					continue;

				const float ratioDist = dist2 / dist1;
				const float ratioOctave = m_pCurrentKeyFrame->m_pPLevelInfo->m_vScaleFactors[kp1.octave] / pKF2->m_pPLevelInfo->m_vScaleFactors[kp2.octave];

				/*if(fabs(ratioDist-ratioOctave)>ratioFactor)
				continue;*/
				if (ratioDist*ratioFactor<ratioOctave || ratioDist>ratioOctave*ratioFactor)
					continue;

				// Triangulation is succesfull
				MapPoint* pMP = new MapPoint(x3D, m_pCurrentKeyFrame, m_pMap);

				pMP->AddObservation(m_pCurrentKeyFrame, idx1);
				pMP->AddObservation(pKF2, idx2);

				m_pCurrentKeyFrame->AddMapPoint(pMP, idx1);
				pKF2->AddMapPoint(pMP, idx2);

				pMP->ComputeDistinctiveDescriptors();
				pMP->UpdateNormalAndDepth();

				m_pMap->AddMapPoint(pMP);
				m_lpRecentAddedMapPoints.push_back(pMP);

				nnew++;
			}
		}
	}

	cv::Mat LocalMapping::ComputeF12(KeyFrame *&pKF1, KeyFrame *&pKF2) {

		cv::Mat R1w = pKF1->GetRotation();
		cv::Mat t1w = pKF1->GetTranslation();
		cv::Mat R2w = pKF2->GetRotation();
		cv::Mat t2w = pKF2->GetTranslation();

		cv::Mat R12 = R1w*R2w.t();
		cv::Mat t12 = -R1w*R2w.t()*t2w + t1w;

		cv::Mat t12x = SkewSymmetricMatrix(t12);

		const cv::Mat &K1 = pKF1->m_pCameraInfo->m_K;
		const cv::Mat &K2 = pKF2->m_pCameraInfo->m_K;

		cv::Mat E = t12x*R12;

		return K1.t().inv()*t12x*R12*K2.inv();
	}

	cv::Mat LocalMapping::SkewSymmetricMatrix(const cv::Mat &v) {

		return (cv::Mat_<float>(3, 3) << 0, -v.at<float>(2), v.at<float>(1),
			v.at<float>(2), 0, -v.at<float>(0),
			-v.at<float>(1), v.at<float>(0), 0);
	}

	void LocalMapping::SearchInNeighbors() {
		int nn = 10;

		const vector<KeyFrame*> vpNeighKFs = m_pCoGraph->GetBestCovisibilityKeyFrames(m_pCurrentKeyFrame, nn);


		vector<KeyFrame*> vpTargetKFs;
		for (vector<KeyFrame*>::const_iterator vit = vpNeighKFs.begin(), vend = vpNeighKFs.end(); vit != vend; vit++) {
			
			KeyFrame* pKFi = *vit; 

			if (pKFi->isBad() || pKFi->m_nFuseTargetForKF == m_pCurrentKeyFrame->m_nFId)
				continue;

			vpTargetKFs.push_back(pKFi);
			pKFi->m_nFuseTargetForKF = m_pCurrentKeyFrame->m_nFId;


			// Extend to some second neighbors
			const vector<KeyFrame*> vpSecondNeighKFs = m_pCoGraph->GetBestCovisibilityKeyFrames(pKFi, 5);
			for (vector<KeyFrame*>::const_iterator vit2 = vpSecondNeighKFs.begin(), vend2 = vpSecondNeighKFs.end(); vit2 != vend2; vit2++) {
				KeyFrame* pKFi2 = *vit2;
				if (pKFi2->isBad() || pKFi2->m_nFuseTargetForKF == m_pCurrentKeyFrame->m_nFId || pKFi2->m_nFId == m_pCurrentKeyFrame->m_nFId)
					continue;
				vpTargetKFs.push_back(pKFi2);
			}
		}


		// Search matches by projection from current KF in target KFs
		ORBmatcher matcher;
		vector<MapPoint*> vpMapPointMatches = m_pCurrentKeyFrame->GetMapPointMatches();
		for (vector<KeyFrame*>::iterator vit = vpTargetKFs.begin(), vend = vpTargetKFs.end(); vit != vend; vit++) {
			KeyFrame* pKFi = *vit;
			matcher.Fuse(pKFi, vpMapPointMatches);
		}

		// Search matches by projection from target KFs in current KF
		vector<MapPoint*> vpFuseCandidates;
		vpFuseCandidates.reserve(vpTargetKFs.size()*vpMapPointMatches.size());

		for (vector<KeyFrame*>::iterator vitKF = vpTargetKFs.begin(), vendKF = vpTargetKFs.end(); vitKF != vendKF; vitKF++) {
			KeyFrame* pKFi = *vitKF;

			vector<MapPoint*> vpMapPointsKFi = pKFi->GetMapPointMatches();

			for (vector<MapPoint*>::iterator vitMP = vpMapPointsKFi.begin(), vendMP = vpMapPointsKFi.end(); vitMP != vendMP; vitMP++) {
				MapPoint* pMP = *vitMP;
				if (!pMP)
					continue;
				if (pMP->isBad() || pMP->m_nFuseCandidateForKF == m_pCurrentKeyFrame->m_nFId)
					continue;
				pMP->m_nFuseCandidateForKF = m_pCurrentKeyFrame->m_nFId;
				vpFuseCandidates.push_back(pMP);
			}
		}

		matcher.Fuse(m_pCurrentKeyFrame, vpFuseCandidates);


		// Update points
		vpMapPointMatches = m_pCurrentKeyFrame->GetMapPointMatches();
		for (size_t i = 0, iend = vpMapPointMatches.size(); i < iend; i++) {
			MapPoint* pMP = vpMapPointMatches[i];
			if (pMP) {
				if (!pMP->isBad()) {
					pMP->ComputeDistinctiveDescriptors();
					pMP->UpdateNormalAndDepth();
				}
			}
		}

		// Update connections in covisibility graph
		m_pCoGraph->UpdateConnections(m_pCurrentKeyFrame);
		m_pSpanTree->UpdateConnections(m_pCurrentKeyFrame);
	}

	void LocalMapping::KeyFrameCulling() {

		// Check redundant keyframes (only local keyframes)
		// A keyframe is considered redundant if the 90% of the MapPoints it sees, are seen
		// in at least other 3 keyframes (in the same or finer scale)
		// We only consider close stereo points

		vector<KeyFrame*> vpLocalKeyFrames = m_pCoGraph->GetVectorCovisibleKeyFrames(m_pCurrentKeyFrame);

		for (vector<KeyFrame*>::iterator vit = vpLocalKeyFrames.begin(), vend = vpLocalKeyFrames.end(); vit != vend; vit++) {
			KeyFrame* pKF = *vit;
			if (pKF->m_nKFId == 0)
				continue;
			const vector<MapPoint*> vpMapPoints = pKF->GetMapPointMatches();

			const int thObs = 3;
			int nRedundantObservations = 0;
			int nMPs = 0;
			for (size_t i = 0, iend = vpMapPoints.size(); i<iend; i++) {
				MapPoint* pMP = vpMapPoints[i];
				if (pMP) {
					if (!pMP->isBad()) {
						
						if (pKF->m_vfDepth[i] > pKF->m_fThDepth || pKF->m_vfDepth[i] < 0)
							continue;
						
						nMPs++;
						if (pMP->Observations()>thObs) {

							const int &scaleLevel = pKF->m_vKeysUn[i].octave;
							const map<KeyFrame*, size_t> observations = pMP->GetObservations();
							int nObs = 0;
							for (map<KeyFrame*, size_t>::const_iterator mit = observations.begin(), mend = observations.end(); mit != mend; mit++) {
								KeyFrame* pKFi = mit->first;
								if (pKFi == pKF)
									continue;
								const int &scaleLeveli = pKFi->m_vKeysUn[mit->second].octave;

								if (scaleLeveli <= scaleLevel + 1) {
									nObs++;
									if (nObs >= thObs)
										break;
								}
							}
							if (nObs >= thObs) {
								nRedundantObservations++;
							}
						}
					}
				}
			}

			if (nRedundantObservations>0.9*nMPs) {
				pKF->SetBadFlag(m_pCoGraph, m_pSpanTree, m_pKeyFrameDB, m_pMap);
			}
		}
	}



	void LocalMapping::RequestStop() {
		unique_lock<mutex> lock(m_MutexStop);
		m_bStopRequested = true;
		unique_lock<mutex> lock2(m_MutexNewKFs);
		m_bAbortBA = true;
	}

	bool LocalMapping::Stop() {
		unique_lock<mutex> lock(m_MutexStop);
		if (m_bStopRequested && !m_bNotStop) {
			m_bStopped = true;
			cout << "Local Mapping STOP" << endl;
			return true;
		}
		return false;
	}

	bool LocalMapping::isStopped() {
		unique_lock<mutex> lock(m_MutexStop);
		return m_bStopped;
	}

	bool LocalMapping::stopRequested() {
		unique_lock<mutex> lock(m_MutexStop);
		return m_bStopRequested;
	}

	bool LocalMapping::AcceptKeyFrames() {
		unique_lock<mutex> lock(m_MutexAccept);
		return m_bAcceptKeyFrames;
	}

	void LocalMapping::SetAcceptKeyFrames(bool flag) {
		unique_lock<mutex> lock(m_MutexAccept);
		m_bAcceptKeyFrames = flag;
	}

	bool LocalMapping::SetNotStop(bool flag) {
		unique_lock<mutex> lock(m_MutexStop);

		if (flag && m_bStopped)
			return false;

		m_bNotStop = flag;

		return true;
	}

	void LocalMapping::InterruptBA() {
		m_bAbortBA = true;
	}

	void LocalMapping::RequestFinish() {
		unique_lock<mutex> lock(m_MutexFinish);
		m_bFinishRequested = true;
	}

	bool LocalMapping::CheckFinish() {
		unique_lock<mutex> lock(m_MutexFinish);
		return m_bFinishRequested;
	}

	void LocalMapping::SetFinish() {
		unique_lock<mutex> lock(m_MutexFinish);
		m_bFinished = true;
		unique_lock<mutex> lock2(m_MutexStop);
		m_bStopped = true;
	}

	bool LocalMapping::isFinished() {
		unique_lock<mutex> lock(m_MutexFinish);
		return m_bFinished;
	}
	void LocalMapping::Release() {
		unique_lock<mutex> lock(m_MutexStop);
		unique_lock<mutex> lock2(m_MutexFinish);
		if (m_bFinished)
			return;
		m_bStopped = false;
		m_bStopRequested = false;
		for (list<KeyFrame*>::iterator lit = m_lpNewKeyFrames.begin(), lend = m_lpNewKeyFrames.end(); lit != lend; lit++)
			delete *lit;
		m_lpNewKeyFrames.clear();

		cout << "Local Mapping RELEASE" << endl;
	}



	void LocalMapping::RequestReset() {
		{
			unique_lock<mutex> lock(m_MutexReset);
			m_bResetRequested = true;
		}

		while (1) {
			{
				unique_lock<mutex> lock2(m_MutexReset);
				if (!m_bResetRequested)
					break;
			}
			Sleep(3);
		}
	}

	void LocalMapping::ResetIfRequested() {
		unique_lock<mutex> lock(m_MutexReset);
		if (m_bResetRequested) {
			m_lpNewKeyFrames.clear();
			m_lpRecentAddedMapPoints.clear();
			m_bResetRequested = false;
		}
	}
} // namespace SLAMRecon