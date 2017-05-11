/**
* This file is part of ORB-SLAM2.
*
* Copyright (C) 2014-2016 Raúl Mur-Artal <raulmur at unizar dot es> (University of Zaragoza)
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

#include "ORBmatcher.h"
#include "../SLAM/MapPoint.h"
#include "../SLAM/KeyFrame.h"
#include "ORBVocabulary.h"

namespace SLAMRecon {

	
	const int ORBmatcher::TH_HIGH = 100;
	const int ORBmatcher::TH_LOW = 50;
	
	const int ORBmatcher::HISTO_LENGTH = 30;

	ORBmatcher::ORBmatcher(float nnratio, bool checkOri) : mfNNratio(nnratio), mbCheckOrientation(checkOri)
	{
	}

	ORBmatcher::~ORBmatcher() {

	}

	int ORBmatcher::SearchByProjection(Frame &CurrentFrame, const Frame &LastFrame, const float th) {

		int nmatches = 0;

		// 为了检查旋转一致性
		vector<int> rotHist[HISTO_LENGTH];
		for (int i = 0; i < HISTO_LENGTH; i++)
			rotHist[i].reserve(500);
		const float factor = 1.0f / HISTO_LENGTH;

		// 
		const cv::Mat Rcw = CurrentFrame.m_Transformation.rowRange(0, 3).colRange(0, 3);
		const cv::Mat tcw = CurrentFrame.m_Transformation.rowRange(0, 3).col(3);
		const cv::Mat twc = -Rcw.t()*tcw;

		const cv::Mat Rlw = LastFrame.m_Transformation.rowRange(0, 3).colRange(0, 3);
		const cv::Mat tlw = LastFrame.m_Transformation.rowRange(0, 3).col(3);
		const cv::Mat tlc = Rlw*twc + tlw; //当前相机中心点在前一个相机坐标系中的位置

		//const bool bForward = tlc.at<float>(2) > CurrentFrame.mb;
		//const bool bBackward = -tlc.at<float>(2) > CurrentFrame.mb;
		// Current Frame相对Last Frame是前进还是后退
		//Hao:(这里实际上有点问题，经测试tlc的值一般为+-零点零(零)几，所以bForward和bBackward始终为false?)
		const bool bForward = tlc.at<float>(2) > 40;
		const bool bBackward = -tlc.at<float>(2) > 40;

		for (int i = 0; i < LastFrame.m_nKeys; i++) {
			MapPoint* pMP = LastFrame.m_vpMapPoints[i];

			if (pMP) {
				if (!LastFrame.m_vbOutlier[i]) {

					// Last Frame上的MapPoint投影到Current Frame平面上
					cv::Mat x3Dw = pMP->GetWorldPos();
					cv::Mat x3Dc = Rcw*x3Dw + tcw;

					const float xc = x3Dc.at<float>(0);
					const float yc = x3Dc.at<float>(1);
					const float invzc = 1.0 / x3Dc.at<float>(2);

					if (invzc < 0)
						continue;

					//找出来lastframe上有效的mappoint在currentframe上的坐标位置，为u、v
					float u = CurrentFrame.m_pCameraInfo->m_fx*xc*invzc + CurrentFrame.m_pCameraInfo->m_cx;
					float v = CurrentFrame.m_pCameraInfo->m_fy*yc*invzc + CurrentFrame.m_pCameraInfo->m_cy;

					// 是否在Current Frame图片边界内
					if (u<CurrentFrame.m_pCameraInfo->m_nMinX || u>CurrentFrame.m_pCameraInfo->m_nMaxX)
						continue;
					if (v<CurrentFrame.m_pCameraInfo->m_nMinY || v>CurrentFrame.m_pCameraInfo->m_nMaxY)
						continue;

					// 根据Scale Level求查找半径，Level越小，代表查找的原图越大
					int nLastOctave = LastFrame.m_vKeys[i].octave;

					float radius = th*CurrentFrame.m_pPLevelInfo->m_vScaleFactors[nLastOctave];

					vector<size_t> vIndices2;

					if (bForward)
						vIndices2 = CurrentFrame.GetFeaturesInArea(u, v, radius, nLastOctave);
					else if (bBackward)
						vIndices2 = CurrentFrame.GetFeaturesInArea(u, v, radius, 0, nLastOctave);
					else
						vIndices2 = CurrentFrame.GetFeaturesInArea(u, v, radius, nLastOctave - 1, nLastOctave + 1);

					if (vIndices2.empty())
						continue;

					const cv::Mat dMP = pMP->GetDescriptor();

					int bestDist = 256;
					int bestIdx2 = -1;

					for (vector<size_t>::const_iterator vit = vIndices2.begin(), vend = vIndices2.end(); vit != vend; vit++) {

						// 如果该查找的KeyPoint已经有匹配的MapPoint，实际上的MapPoint，不是临时的MapPoint，那就不进行处理
						// 但是如果匹配上的临时的MapPoint，这个是可以被后面的匹配更新掉的
						const size_t i2 = *vit;
						if (CurrentFrame.m_vpMapPoints[i2])
							if (CurrentFrame.m_vpMapPoints[i2]->Observations() > 0)
								continue;

						const cv::Mat &d = CurrentFrame.m_Descriptors.row(i2);

						const int dist = DescriptorDistance(dMP, d);

						if (dist < bestDist) {
							bestDist = dist;
							bestIdx2 = i2;
						}
					}

					if (bestDist <= TH_HIGH) {
						CurrentFrame.m_vpMapPoints[bestIdx2] = pMP;
						nmatches++;

						if (mbCheckOrientation) {
							float rot = LastFrame.m_vKeysUn[i].angle - CurrentFrame.m_vKeysUn[bestIdx2].angle;
							if (rot < 0.0)
								rot += 360.0f;
							int bin = round(rot*factor);
							if (bin == HISTO_LENGTH)
								bin = 0;
							assert(bin >= 0 && bin < HISTO_LENGTH);
							rotHist[bin].push_back(bestIdx2);
						}
					}
				}
			}
		}

		if (mbCheckOrientation) {
			int ind1 = -1;
			int ind2 = -1;
			int ind3 = -1;

			ComputeThreeMaxima(rotHist, HISTO_LENGTH, ind1, ind2, ind3);

			for (int i = 0; i < HISTO_LENGTH; i++) {
				if (i != ind1 && i != ind2 && i != ind3) {
					for (size_t j = 0, jend = rotHist[i].size(); j < jend; j++) {
						CurrentFrame.m_vpMapPoints[rotHist[i][j]] = static_cast<MapPoint*>(NULL);
						nmatches--;
					}
				}
			}
		}

		return nmatches;
	}

	int ORBmatcher::SearchByProjection(Frame &F, const vector<MapPoint*> &vpMapPoints, const float th) {

		int nmatches = 0;

		const bool bFactor = th != 1.0;

		// 变量LocalMap中的MapPoint
		for (size_t iMP = 0; iMP < vpMapPoints.size(); iMP++) {

			MapPoint* pMP = vpMapPoints[iMP];

			// 该点不能满足条件，直接返回，isInFrustum中条件
			if (!pMP->m_bTrackInView)
				continue;

			if (pMP->isBad())
				continue;

			const int &nPredictedLevel = pMP->m_nTrackScaleLevel;

			// 根据视线夹角来判断查找半径
			float r = RadiusByViewingCos(pMP->m_TrackViewCos);

			// 乘以上面传入的系数
			if (bFactor)
				r *= th;

			// 判断之前判断的是不是合理，本身代码是有问题的，不能保证求得的Scale Level是合法的
			if (nPredictedLevel >= F.m_pPLevelInfo->m_vScaleFactors.size()) 
				continue;

			// 找到Frame在该范围内的KeyPoint的index集合，同时限定了取得KeyPoint的Level
			const vector<size_t> vIndices =
				F.GetFeaturesInArea(pMP->m_TrackProjX, pMP->m_TrackProjY, r*F.m_pPLevelInfo->m_vScaleFactors[nPredictedLevel], nPredictedLevel - 1, nPredictedLevel);

			if (vIndices.empty())
				continue;

			const cv::Mat MPdescriptor = pMP->GetDescriptor();

			int bestDist = 256;
			int bestLevel = -1;
			int bestDist2 = 256;
			int bestLevel2 = -1;
			int bestIdx = -1;

			// 为该MapPoint查找最好的一个匹配的KeyPoint
			for (vector<size_t>::const_iterator vit = vIndices.begin(), vend = vIndices.end(); vit != vend; vit++) {
				const size_t idx = *vit;

				// 判断该index上的MapPoint是否已经匹配上某个合法的MapPoint了，如果匹配上就不进行处理，但是不包括那些新建的临时的MapPoint
				if (F.m_vpMapPoints[idx])
					if (F.m_vpMapPoints[idx]->Observations() > 0)
						continue;

				const cv::Mat &d = F.m_Descriptors.row(idx);

				const int dist = DescriptorDistance(MPdescriptor, d);

				if (dist < bestDist) {
					bestDist2 = bestDist;
					bestDist = dist;
					bestLevel2 = bestLevel;
					bestLevel = F.m_vKeysUn[idx].octave;
					bestIdx = idx;
				} else if (dist<bestDist2) {
					bestLevel2 = F.m_vKeysUn[idx].octave;
					bestDist2 = dist;
				}
			}

			// 如果该相似度满足一定条件，则认为真正匹配上，同时还要保证第一和第二相似的KeyPoint处于同一个Level
			if (bestDist <= TH_HIGH) {
				if (bestLevel == bestLevel2 && bestDist > mfNNratio*bestDist2)
					continue;

				F.m_vpMapPoints[bestIdx] = pMP;
				nmatches++;
			}
		}

		return nmatches;
	}

	float ORBmatcher::RadiusByViewingCos(const float &viewCos) {
		if (viewCos > 0.998)
			return 2.5;
		else
			return 4.0;
	}

	int ORBmatcher::SearchByBoW(KeyFrame* pKF, Frame &F, vector<MapPoint*> &vpMapPointMatches) {

		// 得到KeyFrame上的MapPoint列表，当然其中有NULL的MapPoint
		const vector<MapPoint*> vpMapPointsKF = pKF->GetMapPointMatches();

		// 对返回的MapPoint列表进行初始化，顺序对应F中的KeyPoint的顺序，初始值为NULL
		vpMapPointMatches = vector<MapPoint*>(F.m_nKeys, static_cast<MapPoint*>(NULL));

		// 得到KeyFrame的FeatureVector，加速遍历，只对比在同一个NodeId上的KeyPoint，加速查询，因为FeatureVector和KeyPoint的descriptor是一一对应的
		const DBoW2::FeatureVector &vFeatVecKF = pKF->m_FeatVec;

		// 用于统计匹配的KyePoint的相对偏移的angle角度，因为按照一般情况来说，偏移角度应该服从一般的规律，所以的应该都差不多
		vector<int> rotHist[HISTO_LENGTH];
		for (int i = 0; i < HISTO_LENGTH; i++)
			rotHist[i].reserve(500);
		const float factor = 1.0f / HISTO_LENGTH;

		// 分别得到FeatureVector迭代器
		DBoW2::FeatureVector::const_iterator KFit = vFeatVecKF.begin();
		DBoW2::FeatureVector::const_iterator Fit = F.m_FeatVec.begin();
		DBoW2::FeatureVector::const_iterator KFend = vFeatVecKF.end();
		DBoW2::FeatureVector::const_iterator Fend = F.m_FeatVec.end();

		int nmatches = 0;

		// 对FeatureVector的进行遍历
		while (KFit != KFend && Fit != Fend) {
		
			// KeyFrame 和 Frame 都包含某个VocTree的NodeId，first代表NodeId
			if (KFit->first == Fit->first) {
			
				// 在KeyFrame上该NodeId对应的KeyPoint（MapPoint）的index 集合
				const vector<unsigned int> vIndicesKF = KFit->second; 

				// 在Frame上该NodeId对应的KeyPoint（MapPoint）的index 集合
				const vector<unsigned int> vIndicesF = Fit->second;

				// 对两个index进行遍历，查询对某个有效的KeyFrame的MapPoint来说，是否存在一个与之匹配的Frame上的MapPoint（KeyPoint的descriptor必须满足一定条件）
				for (size_t iKF = 0; iKF < vIndicesKF.size(); iKF++) {
				
					// KeyFrame上某个MapPoint的index
					const unsigned int realIdxKF = vIndicesKF[iKF]; 

					// 得到该有效的MapPoint对象
					MapPoint* pMP = vpMapPointsKF[realIdxKF];

					if (!pMP)
						continue;
					if (pMP->isBad())
						continue;

					// 该MapPoint对应的KeyPoint的descriptor，256 bit
					const cv::Mat &dKF = pKF->m_Descriptors.row(realIdxKF);

					// 遍历Frame该NodeId上的所有的KeyPoint，求与上面KeyFrame的descriptor之间的最小的和第二小的汉明距离

					// 一共256位的descriptor,对一个keypoint来说，所有最大汉明距离可以设置为256
					// 汉明距离其实是两个descriptor之间不同的bit的个数
					int bestDist1 = 256; 
					int bestIdxF = -1;
					int bestDist2 = 256;

					for (size_t iF = 0; iF < vIndicesF.size(); iF++) {
						const unsigned int realIdxF = vIndicesF[iF];

						if (vpMapPointMatches[realIdxF]) // 如果该节点已经有内容，说明已经被找到，跳过
							continue;

						const cv::Mat &dF = F.m_Descriptors.row(realIdxF);

						const int dist = DescriptorDistance(dKF, dF); // 计算汉明距离

						if (dist < bestDist1) {
							bestDist2 = bestDist1;
							bestDist1 = dist;
							bestIdxF = realIdxF;
						} else if (dist < bestDist2)
							bestDist2 = dist;
					}

					if (bestDist1 <= TH_LOW) {

						if (static_cast<float>(bestDist1) < mfNNratio*static_cast<float>(bestDist2)) {

							vpMapPointMatches[bestIdxF] = pMP;

							const cv::KeyPoint &kp = pKF->m_vKeysUn[realIdxKF];

							if (mbCheckOrientation) {
								float rot = kp.angle - F.m_vKeys[bestIdxF].angle;
								if (rot < 0.0)
									rot += 360.0f;
								int bin = round(rot*factor);
								if (bin == HISTO_LENGTH)
									bin = 0;
								assert(bin >= 0 && bin < HISTO_LENGTH);
								rotHist[bin].push_back(bestIdxF);
							}
							nmatches++;
						}
					}

				}

				KFit++;
				Fit++;
			} else if (KFit->first < Fit->first) {
				KFit = vFeatVecKF.lower_bound(Fit->first);
			} else {
				Fit = F.m_FeatVec.lower_bound(KFit->first);
			}
		}


		if (mbCheckOrientation) {
			int ind1 = -1;
			int ind2 = -1;
			int ind3 = -1;

			ComputeThreeMaxima(rotHist, HISTO_LENGTH, ind1, ind2, ind3);

			for (int i = 0; i < HISTO_LENGTH; i++) {
				if (i == ind1 || i == ind2 || i == ind3)
					continue;
				for (size_t j = 0, jend = rotHist[i].size(); j < jend; j++) {
					vpMapPointMatches[rotHist[i][j]] = static_cast<MapPoint*>(NULL);
					nmatches--;
				}
			}
		}

		return nmatches;
	}

	int ORBmatcher::SearchByBoW(KeyFrame *pKF1, KeyFrame *pKF2, vector<MapPoint *> &vpMatches12) {

		// 获得KeyFrame1的相关KeyPoint，FeatureVector，MapPoint以及Descriptors信息
		const vector<cv::KeyPoint> &vKeysUn1 = pKF1->m_vKeysUn;
		const DBoW2::FeatureVector &vFeatVec1 = pKF1->m_FeatVec;
		const vector<MapPoint*> vpMapPoints1 = pKF1->GetMapPointMatches();
		const cv::Mat &Descriptors1 = pKF1->m_Descriptors;

		// 获得KeyFrame2的相关KeyPoint，FeatureVector，MapPoint以及Descriptors信息
		const vector<cv::KeyPoint> &vKeysUn2 = pKF2->m_vKeysUn;
		const DBoW2::FeatureVector &vFeatVec2 = pKF2->m_FeatVec;
		const vector<MapPoint*> vpMapPoints2 = pKF2->GetMapPointMatches();
		const cv::Mat &Descriptors2 = pKF2->m_Descriptors;

		// 返回的匹配KeyFrame1的MapPoint的KeyFrame2的MapPoint的值
		vpMatches12 = vector<MapPoint*>(vpMapPoints1.size(), static_cast<MapPoint*>(NULL));

		// 标志位，KeyFrame2上的某个MapPoint是不是已经被匹配过了，只能匹配一次，一对一关系
		vector<bool> vbMatched2(vpMapPoints2.size(), false); 

		// 用于统计角度关系
		vector<int> rotHist[HISTO_LENGTH];
		for (int i = 0; i < HISTO_LENGTH; i++)
			rotHist[i].reserve(500);
		const float factor = 1.0f / HISTO_LENGTH;

		int nmatches = 0;

		DBoW2::FeatureVector::const_iterator f1it = vFeatVec1.begin();
		DBoW2::FeatureVector::const_iterator f2it = vFeatVec2.begin();
		DBoW2::FeatureVector::const_iterator f1end = vFeatVec1.end();
		DBoW2::FeatureVector::const_iterator f2end = vFeatVec2.end();

		while (f1it != f1end && f2it != f2end) {

			if (f1it->first == f2it->first) {

				// 对KeyFrame1上的某个NodeId下的MapPoint进行遍历，对应遍历KeyFrame2相同NodeId下的MapPoint
				for (size_t i1 = 0, iend1 = f1it->second.size(); i1 < iend1; i1++) {

					const size_t idx1 = f1it->second[i1];

					MapPoint* pMP1 = vpMapPoints1[idx1];

					if (!pMP1)
						continue;
					if (pMP1->isBad())
						continue;

					const cv::Mat &d1 = Descriptors1.row(idx1);

					int bestDist1 = 256;
					int bestIdx2 = -1;
					int bestDist2 = 256;

					// 对KeyFrame2上的某个NodeId下的MapPoint进行遍历
					for (size_t i2 = 0, iend2 = f2it->second.size(); i2 < iend2; i2++) {
						const size_t idx2 = f2it->second[i2];

						MapPoint* pMP2 = vpMapPoints2[idx2];

						if (vbMatched2[idx2] || !pMP2)
							continue;

						if (pMP2->isBad())
							continue;

						const cv::Mat &d2 = Descriptors2.row(idx2);

						int dist = DescriptorDistance(d1, d2);

						if (dist < bestDist1) {
							bestDist2 = bestDist1;
							bestDist1 = dist;
							bestIdx2 = idx2;
						} else if (dist < bestDist2) {
							bestDist2 = dist;
						}
					}

					if (bestDist1 < TH_LOW) {

						if (static_cast<float>(bestDist1) < mfNNratio*static_cast<float>(bestDist2)) {

							vpMatches12[idx1] = vpMapPoints2[bestIdx2];
							vbMatched2[bestIdx2] = true;

							if (mbCheckOrientation) {
								float rot = vKeysUn1[idx1].angle - vKeysUn2[bestIdx2].angle;
								if (rot < 0.0)
									rot += 360.0f;
								int bin = round(rot*factor);
								if (bin == HISTO_LENGTH)
									bin = 0;
								assert(bin >= 0 && bin < HISTO_LENGTH);
								rotHist[bin].push_back(idx1);
							}
							nmatches++;
						}
					}
				}

				f1it++;
				f2it++;
			}
			else if (f1it->first < f2it->first) {
				f1it = vFeatVec1.lower_bound(f2it->first);
			}
			else {
				f2it = vFeatVec2.lower_bound(f1it->first);
			}
		}

		if (mbCheckOrientation) {
			int ind1 = -1;
			int ind2 = -1;
			int ind3 = -1;

			ComputeThreeMaxima(rotHist, HISTO_LENGTH, ind1, ind2, ind3);

			for (int i = 0; i < HISTO_LENGTH; i++) {
				if (i == ind1 || i == ind2 || i == ind3)
					continue;
				for (size_t j = 0, jend = rotHist[i].size(); j < jend; j++) {
					vpMatches12[rotHist[i][j]] = static_cast<MapPoint*>(NULL);
					nmatches--;
				}
			}
		}

		return nmatches;
	}

	int ORBmatcher::SearchByProjection(Frame &CurrentFrame, KeyFrame *pKF, const set<MapPoint*> &sAlreadyFound, const float th, const int ORBdist) {

		int nmatches = 0;

		const cv::Mat Rcw = CurrentFrame.m_Transformation.rowRange(0, 3).colRange(0, 3);  // camera pose 是优化器优化后的 pose
		const cv::Mat tcw = CurrentFrame.m_Transformation.rowRange(0, 3).col(3);
		const cv::Mat Ow = -Rcw.t()*tcw;  // 当前帧相机坐标系原点在世界坐标系中的位置

		vector<int> rotHist[HISTO_LENGTH];
		for (int i = 0; i < HISTO_LENGTH; i++)
			rotHist[i].reserve(500);
		const float factor = 1.0f / HISTO_LENGTH;

		const vector<MapPoint*> vpMPs = pKF->GetMapPointMatches();

		for (size_t i = 0, iend = vpMPs.size(); i < iend; i++) {

			MapPoint* pMP = vpMPs[i];

			if (pMP) {

				// KeyFrame已经匹配上的
				if (!pMP->isBad() && !sAlreadyFound.count(pMP)) {  

					// KeyFrame 上存在的 MapPoint 的世界坐标位置
					cv::Mat x3Dw = pMP->GetWorldPos();  
					// KeyFrame 上存在的 MapPoint 的在 current Frame 相机坐标位置
					cv::Mat x3Dc = Rcw*x3Dw + tcw;   

					// KeyFrame 上存在的 MapPoint 的在 current Frame 相机坐标位置， 单独 x、y、z
					const float xc = x3Dc.at<float>(0);
					const float yc = x3Dc.at<float>(1);
					const float invzc = 1.0 / x3Dc.at<float>(2);

					// KeyFrame 上存在的 MapPoint 的在 current Frame 像素坐标位置， 单独 x、y、z
					const float u = CurrentFrame.m_pCameraInfo->m_fx*xc*invzc + CurrentFrame.m_pCameraInfo->m_cx;
					const float v = CurrentFrame.m_pCameraInfo->m_fy*yc*invzc + CurrentFrame.m_pCameraInfo->m_cy;

					if (u<CurrentFrame.m_pCameraInfo->m_nMinX || u>CurrentFrame.m_pCameraInfo->m_nMaxX)
						continue;
					if (v<CurrentFrame.m_pCameraInfo->m_nMinY || v>CurrentFrame.m_pCameraInfo->m_nMaxY)
						continue;

					// 判断距离是不是满足条件
					cv::Mat PO = x3Dw - Ow;
					float dist3D = cv::norm(PO);

					const float maxDistance = pMP->GetMaxDistanceInvariance();
					const float minDistance = pMP->GetMinDistanceInvariance();

					if (dist3D<minDistance || dist3D>maxDistance)
						continue;

					// 查询当前点在current frame中的level
					int nPredictedLevel = pMP->PredictScale(dist3D, CurrentFrame.m_pPLevelInfo->m_fLogScaleFactor);

					// 判断之前判断的是不是合理，本身代码是有问题的，不能保证求得的Scale Level是合法的
					if (nPredictedLevel >= CurrentFrame.m_pPLevelInfo->m_vScaleFactors.size())
						continue;

					// Search in a window，在此半径范围内查找候选KeyPoint点，descriptors距离小于一定阈值说明该点也可以是Match上的MapPoint。
					const float radius = th*CurrentFrame.m_pPLevelInfo->m_vScaleFactors[nPredictedLevel];

					const vector<size_t> vIndices2 = CurrentFrame.GetFeaturesInArea(u, v, radius, nPredictedLevel - 1, nPredictedLevel + 1);
					if (vIndices2.empty())
						continue;

					const cv::Mat dMP = pMP->GetDescriptor();

					int bestDist = 256;
					int bestIdx2 = -1;

					for (vector<size_t>::const_iterator vit = vIndices2.begin(); vit != vIndices2.end(); vit++) {
						const size_t i2 = *vit;
						if (CurrentFrame.m_vpMapPoints[i2])
							continue;

						const cv::Mat &d = CurrentFrame.m_Descriptors.row(i2);

						const int dist = DescriptorDistance(dMP, d);

						if (dist < bestDist) {
							bestDist = dist;
							bestIdx2 = i2;
						}
					}

					// 从候选的KeyPoint中选出descript的汉明距离最小的，如果小于该阈值说明该KeyPoint对应KeyFrame的MapPoint
					if (bestDist <= ORBdist) {
						CurrentFrame.m_vpMapPoints[bestIdx2] = pMP;
						nmatches++;

						if (mbCheckOrientation) {
							float rot = pKF->m_vKeysUn[i].angle - CurrentFrame.m_vKeysUn[bestIdx2].angle;
							if (rot < 0.0)
								rot += 360.0f;
							int bin = round(rot*factor);
							if (bin == HISTO_LENGTH)
								bin = 0;
							assert(bin >= 0 && bin < HISTO_LENGTH);
							rotHist[bin].push_back(bestIdx2);
						}
					}

				}
			}
		}

		if (mbCheckOrientation) {
			int ind1 = -1;
			int ind2 = -1;
			int ind3 = -1;

			ComputeThreeMaxima(rotHist, HISTO_LENGTH, ind1, ind2, ind3);

			for (int i = 0; i < HISTO_LENGTH; i++) {
				if (i != ind1 && i != ind2 && i != ind3) {
					for (size_t j = 0, jend = rotHist[i].size(); j < jend; j++) {
						CurrentFrame.m_vpMapPoints[rotHist[i][j]] = NULL;
						nmatches--;
					}
				}
			}
		}

		return nmatches;
	}




	int ORBmatcher::SearchForTriangulation(KeyFrame *pKF1, KeyFrame *pKF2, cv::Mat F12,
		vector<pair<size_t, size_t> > &vMatchedPairs, const bool bOnlyStereo)
	{
		const DBoW2::FeatureVector &vFeatVec1 = pKF1->m_FeatVec;
		const DBoW2::FeatureVector &vFeatVec2 = pKF2->m_FeatVec;

		//Compute epipole in second image
		cv::Mat Cw = pKF1->GetCameraCenter(); // 第一个KeyFrame相机中心点在世界坐标系中的位置
		cv::Mat R2w = pKF2->GetRotation();
		cv::Mat t2w = pKF2->GetTranslation();
		cv::Mat C2 = R2w*Cw + t2w; // 第一个KeyFrame相机中心点在第二个KeyFrame相机坐标系中的位置

		// 求第一个KeyFrame相机中心点在第二个KeyFrame图像坐标系中的位置
		const float invz = 1.0f / C2.at<float>(2);
		const float ex = pKF2->m_pCameraInfo->m_fx*C2.at<float>(0)*invz + pKF2->m_pCameraInfo->m_cx;
		const float ey = pKF2->m_pCameraInfo->m_fy*C2.at<float>(1)*invz + pKF2->m_pCameraInfo->m_cy;

		// Find matches between not tracked keypoints
		// Matching speed-up by ORB Vocabulary
		// Compare only ORB that share the same node

		int nmatches = 0;
		vector<bool> vbMatched2(pKF2->m_nKeys, false);
		vector<int> vMatches12(pKF1->m_nKeys, -1);

		vector<int> rotHist[HISTO_LENGTH];
		for (int i = 0; i < HISTO_LENGTH; i++)
			rotHist[i].reserve(500);

		const float factor = 1.0f / HISTO_LENGTH;

		DBoW2::FeatureVector::const_iterator f1it = vFeatVec1.begin();
		DBoW2::FeatureVector::const_iterator f2it = vFeatVec2.begin();
		DBoW2::FeatureVector::const_iterator f1end = vFeatVec1.end();
		DBoW2::FeatureVector::const_iterator f2end = vFeatVec2.end();

		// 这个策略和上面searchbyBow很相似
		while (f1it != f1end && f2it != f2end) {

			// 在同一个node下才进行计算，减少计算复杂度
			if (f1it->first == f2it->first) {

				for (size_t i1 = 0, iend1 = f1it->second.size(); i1 < iend1; i1++) {

					const size_t idx1 = f1it->second[i1];
					MapPoint* pMP1 = pKF1->GetMapPoint(idx1);

					// If there is already a MapPoint skip
					if (pMP1)
						continue;

					const cv::KeyPoint &kp1 = pKF1->m_vKeysUn[idx1];
					const cv::Mat &d1 = pKF1->m_Descriptors.row(idx1);

					int bestDist = TH_LOW;
					int bestIdx2 = -1;

					for (size_t i2 = 0, iend2 = f2it->second.size(); i2 < iend2; i2++) {
						size_t idx2 = f2it->second[i2];

						MapPoint* pMP2 = pKF2->GetMapPoint(idx2);

						// If we have already matched or there is a MapPoint skip
						if (vbMatched2[idx2] || pMP2)
							continue;

				
						const cv::Mat &d2 = pKF2->m_Descriptors.row(idx2);

						const int dist = DescriptorDistance(d1, d2);

						if (dist > TH_LOW || dist > bestDist)
							continue;

						const cv::KeyPoint &kp2 = pKF2->m_vKeysUn[idx2];

						
						const float distex = ex - kp2.pt.x; 
						const float distey = ey - kp2.pt.y;

						// 计算？？？？？？？？？？？
						if (distex*distex + distey*distey < 100 * pKF2->m_pPLevelInfo->m_vScaleFactors[kp2.octave])
							continue;
						
						// 计算点到对一个极线的距离
						if (CheckDistEpipolarLine(kp1, kp2, F12, pKF2)) {
							bestIdx2 = idx2;
							bestDist = dist;
						}
					}

					if (bestIdx2 >= 0) { 
						const cv::KeyPoint &kp2 = pKF2->m_vKeysUn[bestIdx2];
						vMatches12[idx1] = bestIdx2;
						nmatches++;

						if (mbCheckOrientation) {
							float rot = kp1.angle - kp2.angle;
							if (rot < 0.0)
								rot += 360.0f;
							int bin = round(rot*factor);
							if (bin == HISTO_LENGTH)
								bin = 0;
							assert(bin >= 0 && bin < HISTO_LENGTH);
							rotHist[bin].push_back(idx1);
						}
					}
				}

				f1it++;
				f2it++;
			} else if (f1it->first < f2it->first) {
				f1it = vFeatVec1.lower_bound(f2it->first);
			} else {
				f2it = vFeatVec2.lower_bound(f1it->first);
			}
		}

		if (mbCheckOrientation) {
			int ind1 = -1;
			int ind2 = -1;
			int ind3 = -1;

			ComputeThreeMaxima(rotHist, HISTO_LENGTH, ind1, ind2, ind3);

			for (int i = 0; i < HISTO_LENGTH; i++) {
				if (i == ind1 || i == ind2 || i == ind3)
					continue;
				for (size_t j = 0, jend = rotHist[i].size(); j < jend; j++) {
					vMatches12[rotHist[i][j]] = -1;
					nmatches--;
				}
			}

		}

		vMatchedPairs.clear();
		vMatchedPairs.reserve(nmatches);

		for (size_t i = 0, iend = vMatches12.size(); i < iend; i++)
		{
			if (vMatches12[i] < 0)
				continue;
			vMatchedPairs.push_back(make_pair(i, vMatches12[i]));
		}

		return nmatches;
	}

	int ORBmatcher::Fuse(KeyFrame *pKF, const vector<MapPoint *> &vpMapPoints, const float th) {

		cv::Mat Rcw = pKF->GetRotation();
		cv::Mat tcw = pKF->GetTranslation();

		const float &fx = pKF->m_pCameraInfo->m_fx;
		const float &fy = pKF->m_pCameraInfo->m_fy;
		const float &cx = pKF->m_pCameraInfo->m_cx;
		const float &cy = pKF->m_pCameraInfo->m_cy;
		//const float &bf = pKF->mbf;

		cv::Mat Ow = pKF->GetCameraCenter();

		int nFused = 0;

		const int nMPs = vpMapPoints.size();

		for (int i = 0; i < nMPs; i++) {

			MapPoint* pMP = vpMapPoints[i];

			if (!pMP)
				continue;

			if (pMP->isBad() || pMP->IsInKeyFrame(pKF))
				continue;

			cv::Mat p3Dw = pMP->GetWorldPos();
			cv::Mat p3Dc = Rcw*p3Dw + tcw;

			// Depth must be positive
			if (p3Dc.at<float>(2) < 0.0f)
				continue;

			const float invz = 1 / p3Dc.at<float>(2);
			const float x = p3Dc.at<float>(0)*invz;
			const float y = p3Dc.at<float>(1)*invz;

			const float u = fx*x + cx;
			const float v = fy*y + cy;

			// Point must be inside the image
			if (!pKF->IsInImage(u, v))
				continue;

			//const float ur = u - bf*invz;

			const float maxDistance = pMP->GetMaxDistanceInvariance();
			const float minDistance = pMP->GetMinDistanceInvariance();
			cv::Mat PO = p3Dw - Ow;
			const float dist3D = cv::norm(PO);

			// Depth must be inside the scale pyramid of the image
			if (dist3D<minDistance || dist3D>maxDistance)
				continue;

			// Viewing angle must be less than 60 deg
			cv::Mat Pn = pMP->GetNormal();

			if (PO.dot(Pn) < 0.5*dist3D)
				continue;

			int nPredictedLevel = pMP->PredictScale(dist3D, pKF->m_pPLevelInfo->m_fLogScaleFactor);

			// 判断之前判断的是不是合理，本身代码是有问题的，不能保证求得的Scale Level是合法的
			if (nPredictedLevel >= pKF->m_pPLevelInfo->m_vScaleFactors.size())
				continue;

			// Search in a radius
			const float radius = th*pKF->m_pPLevelInfo->m_vScaleFactors[nPredictedLevel];


			const vector<size_t> vIndices = pKF->GetFeaturesInArea(u, v, radius);

			if (vIndices.empty())
				continue;

			// Match to the most similar keypoint in the radius

			const cv::Mat dMP = pMP->GetDescriptor();

			int bestDist = 256;
			int bestIdx = -1;
			for (vector<size_t>::const_iterator vit = vIndices.begin(), vend = vIndices.end(); vit != vend; vit++) {
				const size_t idx = *vit;

				const cv::KeyPoint &kp = pKF->m_vKeysUn[idx];

				const int &kpLevel = kp.octave;

				if (kpLevel<nPredictedLevel - 1 || kpLevel>nPredictedLevel)
					continue;

				
				const float &kpx = kp.pt.x;
				const float &kpy = kp.pt.y;
				const float ex = u - kpx;
				const float ey = v - kpy;
				const float e2 = ex*ex + ey*ey;

				if (e2*pKF->m_pPLevelInfo->m_vInvLevelSigma2[kpLevel] > 5.99)
					continue;
				
				const cv::Mat &dKF = pKF->m_Descriptors.row(idx);

				const int dist = DescriptorDistance(dMP, dKF);

				if (dist<bestDist) {
					bestDist = dist;
					bestIdx = idx;
				}
			}

			// If there is already a MapPoint replace otherwise add new measurement
			if (bestDist <= TH_LOW) {
				MapPoint* pMPinKF = pKF->GetMapPoint(bestIdx);
				if (pMPinKF) {
					if (!pMPinKF->isBad()) {
						if (pMPinKF->Observations()>pMP->Observations())
							pMP->Replace(pMPinKF);
						else
							pMPinKF->Replace(pMP);
					}
				} else {
					pMP->AddObservation(pKF, bestIdx);
					pKF->AddMapPoint(pMP, bestIdx);
				}
				nFused++;
			}
		}

		return nFused;
	}






	

	int ORBmatcher::SearchBySim3(KeyFrame *pKF1, KeyFrame *pKF2, vector<MapPoint*> &vpMatches12, const cv::Mat &R12, const cv::Mat &t12, const float th) {
		
		const float &fx = pKF1->m_pCameraInfo->m_fx;
		const float &fy = pKF1->m_pCameraInfo->m_fy;
		const float &cx = pKF1->m_pCameraInfo->m_cx;
		const float &cy = pKF1->m_pCameraInfo->m_cy;

		// KeyFrame1相对世界坐标系的旋转矩阵和平移向量
		cv::Mat R1w = pKF1->GetRotation();
		cv::Mat t1w = pKF1->GetTranslation();

		// KeyFrame2相对世界坐标系的旋转矩阵和平移向量
		cv::Mat R2w = pKF2->GetRotation();
		cv::Mat t2w = pKF2->GetTranslation();

		// 反过来求KeyFrame2相对于KeyFrame1的旋转矩阵和平移向量
		cv::Mat R21 = R12.t();
		cv::Mat t21 = -R21*t12;

		// 分别获得两个KeyFrame的MapPoint以及对应的数目
		const vector<MapPoint*> vpMapPoints1 = pKF1->GetMapPointMatches();
		const int N1 = vpMapPoints1.size();

		const vector<MapPoint*> vpMapPoints2 = pKF2->GetMapPointMatches();
		const int N2 = vpMapPoints2.size();

		// 根据传入的匹配的vpMatches12，来构建下面两个对象，ture表示是可以正常匹配上的值
		vector<bool> vbAlreadyMatched1(N1, false);
		vector<bool> vbAlreadyMatched2(N2, false);

		for (int i = 0; i < N1; i++) {
			MapPoint* pMP = vpMatches12[i]; // KeyFrame2上的MapPoint，对应的是KeyFrame1的i位置上MapPoint
			if (pMP) {
				vbAlreadyMatched1[i] = true;
				int idx2 = pMP->GetIndexInKeyFrame(pKF2);
				if (idx2 >= 0 && idx2 < N2)
					vbAlreadyMatched2[idx2] = true;
			}
		}

		// KeyFrame1中正常的MapPoint，同时不包括上面正常匹配的那些MapPoint，匹配上的KeyFrame2中的KeyPoint的index
		vector<int> vnMatch1(N1, -1);
		// KeyFrame2中正常的MapPoint，同时不包括上面正常匹配的那些MapPoint，匹配上的KeyFrame1中的KeyPoint的index
		vector<int> vnMatch2(N2, -1);

		// 遍历KeyFrame1中正常的MapPoint，同时不包括上面正常匹配的那些MapPoint
		// 找到在KeyFrame2中可以匹配上该MapPoint的KeyPoint
		for (int i1 = 0; i1 < N1; i1++) {

			MapPoint* pMP = vpMapPoints1[i1];

			if (!pMP || vbAlreadyMatched1[i1])
				continue;

			if (pMP->isBad())
				continue;

			cv::Mat p3Dw = pMP->GetWorldPos(); // 世界坐标系中位置
			cv::Mat p3Dc1 = R1w*p3Dw + t1w; // KeyFrame1相机坐标系中位置
			cv::Mat p3Dc2 = R21*p3Dc1 + t21; // KeyFrame2相机坐标系中位置

			// 在KeyFrame2相机坐标系，depth必须是正值，不然该点时观察不到的
			if (p3Dc2.at<float>(2) < 0.0)
				continue;

			// 求在KeyFrame1上的该MapPoint投影到KeyFrame2的平面上的二维点的位置
			const float invz = 1.0 / p3Dc2.at<float>(2);
			const float x = p3Dc2.at<float>(0)*invz;
			const float y = p3Dc2.at<float>(1)*invz;

			const float u = fx*x + cx;
			const float v = fy*y + cy;

			// 投影后的二维点必须在KeyFrame的图片边界内
			if (!pKF2->IsInImage(u, v))
				continue;

			// 查看该MapPoint距离KeyFrame2相机坐标系是不是满足该MapPoint的距离限制
			const float maxDistance = pMP->GetMaxDistanceInvariance();
			const float minDistance = pMP->GetMinDistanceInvariance();
			const float dist3D = cv::norm(p3Dc2);

			if (dist3D<minDistance || dist3D>maxDistance)
				continue;

			// 估计该点如果属于该KeyFrame2应该处于金字塔模型的Level
			const int nPredictedLevel = pMP->PredictScale(dist3D, pKF2->m_pPLevelInfo->m_fLogScaleFactor);

			// 判断之前判断的是不是合理，本身代码是有问题的，不能保证求得的Scale Level是合法的
			if (nPredictedLevel >= pKF2->m_pPLevelInfo->m_vScaleFactors.size())
				continue;

			// 在前面求得二维投影的某个半径范围内求相关的KeyPoint
			// Level越大意味着图片越小，如果在原图上进行查找的话半径就需要进行相应的放大
			const float radius = th*pKF2->m_pPLevelInfo->m_vScaleFactors[nPredictedLevel];

			// 得到该范围内的KeyPoint的index的集合
			const vector<size_t> vIndices = pKF2->GetFeaturesInArea(u, v, radius);

			if (vIndices.empty())
				continue;

			// 该MapPoint要与该范围内最相近的KeyPoint进行匹配，即descriptor的汉明距离最小
			const cv::Mat dMP = pMP->GetDescriptor();

			// 对该范围内的KeyPoint进行遍历，并寻找与该MapPoint最匹配的KeyPoint
			int bestDist = INT_MAX;
			int bestIdx = -1;
			for (vector<size_t>::const_iterator vit = vIndices.begin(), vend = vIndices.end(); vit != vend; vit++) {
				const size_t idx = *vit;

				const cv::KeyPoint &kp = pKF2->m_vKeysUn[idx];

				// 该KeyPoint的金字塔的Level和上面估计的Level不能相差太大，最多小1个Level，
				if (kp.octave<nPredictedLevel - 1 || kp.octave>nPredictedLevel)
					continue;

				const cv::Mat &dKF = pKF2->m_Descriptors.row(idx);

				// 计算MapPoint的descriptor和KeyPoint的descriptor的汉明距离
				const int dist = DescriptorDistance(dMP, dKF);

				if (dist < bestDist) {
					bestDist = dist;
					bestIdx = idx;
				}
			}

			// 如果距离小于大的阈值，认为匹配上了，保存在上面的变量中，这个阈值比较宽松
			if (bestDist <= TH_HIGH) {
				vnMatch1[i1] = bestIdx;
			}
		}

		// 同理遍历KeyFrame2中正常的MapPoint，同时不包括上面正常匹配的那些MapPoint
		// 找到在KeyFrame1中可以匹配上该MapPoint的KeyPoint
		// 过程和上面完全一样
		for (int i2 = 0; i2 < N2; i2++) {
			MapPoint* pMP = vpMapPoints2[i2];

			if (!pMP || vbAlreadyMatched2[i2])
				continue;

			if (pMP->isBad())
				continue;

			cv::Mat p3Dw = pMP->GetWorldPos();
			cv::Mat p3Dc2 = R2w*p3Dw + t2w;
			cv::Mat p3Dc1 = R12*p3Dc2 + t12;

			if (p3Dc1.at<float>(2) < 0.0)
				continue;

			const float invz = 1.0 / p3Dc1.at<float>(2);
			const float x = p3Dc1.at<float>(0)*invz;
			const float y = p3Dc1.at<float>(1)*invz;

			const float u = fx*x + cx;
			const float v = fy*y + cy;

			// Point must be inside the image
			if (!pKF1->IsInImage(u, v))
				continue;

			const float maxDistance = pMP->GetMaxDistanceInvariance();
			const float minDistance = pMP->GetMinDistanceInvariance();
			const float dist3D = cv::norm(p3Dc1);

			if (dist3D<minDistance || dist3D>maxDistance)
				continue;

			const int nPredictedLevel = pMP->PredictScale(dist3D, pKF1->m_pPLevelInfo->m_fLogScaleFactor);

			// 判断之前判断的是不是合理，本身代码是有问题的，不能保证求得的Scale Level是合法的
			if (nPredictedLevel >= pKF1->m_pPLevelInfo->m_vScaleFactors.size())
				continue;

			const float radius = th*pKF1->m_pPLevelInfo->m_vScaleFactors[nPredictedLevel];

			const vector<size_t> vIndices = pKF1->GetFeaturesInArea(u, v, radius);

			if (vIndices.empty())
				continue;

			const cv::Mat dMP = pMP->GetDescriptor();

			int bestDist = INT_MAX;
			int bestIdx = -1;
			for (vector<size_t>::const_iterator vit = vIndices.begin(), vend = vIndices.end(); vit != vend; vit++) {

				const size_t idx = *vit;

				const cv::KeyPoint &kp = pKF1->m_vKeysUn[idx];

				if (kp.octave<nPredictedLevel - 1 || kp.octave>nPredictedLevel)
					continue;

				const cv::Mat &dKF = pKF1->m_Descriptors.row(idx);

				const int dist = DescriptorDistance(dMP, dKF);

				if (dist < bestDist) {
					bestDist = dist;
					bestIdx = idx;
				}
			}

			if (bestDist <= TH_HIGH) {
				vnMatch2[i2] = bestIdx;
			}
		}

		// 如果KeyFrame1的MapPoint对应的KeyFrame2中KeyPoint
		// 同时正好该KeyPoint对应的MapPoint也正好对应上KeyFrame1的MapPoint对应KeyPoint
		// 认为这两个MapPoint匹配上
		// nFound为新匹配的MapPoint的对数
		int nFound = 0;

		for (int i1 = 0; i1 < N1; i1++) {
			int idx2 = vnMatch1[i1];

			if (idx2 >= 0) {
				int idx1 = vnMatch2[idx2];
				if (idx1 == i1) {
					vpMatches12[i1] = vpMapPoints2[idx2];
					nFound++;
				}
			}
		}

		return nFound;
	}

	int ORBmatcher::SearchByProjection(KeyFrame* pKF, cv::Mat Scw, const vector<MapPoint*> &vpPoints, vector<MapPoint*> &vpMatched, int th) {

		// 相机内参
		const float &fx = pKF->m_pCameraInfo->m_fx;
		const float &fy = pKF->m_pCameraInfo->m_fy;
		const float &cx = pKF->m_pCameraInfo->m_cx;
		const float &cy = pKF->m_pCameraInfo->m_cy;

		// 转换成R和t
		cv::Mat sRcw = Scw.rowRange(0, 3).colRange(0, 3);
		const float scw = sqrt(sRcw.row(0).dot(sRcw.row(0)));
		cv::Mat Rcw = sRcw / scw;
		cv::Mat tcw = Scw.rowRange(0, 3).col(3) / scw;
		cv::Mat Ow = -Rcw.t()*tcw;

		// 存储已经匹配上的MapPoint
		set<MapPoint*> spAlreadyFound(vpMatched.begin(), vpMatched.end());
		spAlreadyFound.erase(static_cast<MapPoint*>(NULL));

		int nmatches = 0;

		// 遍历候选的MapPoint，从里面寻找新的满足条件的匹配
		for (int iMP = 0, iendMP = vpPoints.size(); iMP < iendMP; iMP++) {

			MapPoint* pMP = vpPoints[iMP];

			// 忽略已经找到的
			if (pMP->isBad() || spAlreadyFound.count(pMP))
				continue;

			// 得到三维点坐标，准备投影到KeyFrame空间
			cv::Mat p3Dw = pMP->GetWorldPos();

			// 转换到相机坐标系
			cv::Mat p3Dc = Rcw*p3Dw + tcw;

			// depth必须是正数，不然该MapPoint在相机后面是看不到的
			if (p3Dc.at<float>(2) < 0.0)
				continue;

			// 投影到图片上，计算图片的坐标
			const float invz = 1 / p3Dc.at<float>(2);
			const float x = p3Dc.at<float>(0)*invz;
			const float y = p3Dc.at<float>(1)*invz;

			const float u = fx*x + cx;
			const float v = fy*y + cy;

			// 投影到图片上的坐标必须在图片边界范围内
			if (!pKF->IsInImage(u, v))
				continue;

			// 到相机中心的距离还要满足该MapPoint范围限制，不然在该KeyFrame也是找不到该MapPoint的
			const float maxDistance = pMP->GetMaxDistanceInvariance();
			const float minDistance = pMP->GetMinDistanceInvariance();
			cv::Mat PO = p3Dw - Ow;
			const float dist = cv::norm(PO);

			if (dist<minDistance || dist>maxDistance)
				continue;

			// 计算到相机中心的方向和该MapPoint平均方向夹角要小于60度，也就是视角要小于60度
			cv::Mat Pn = pMP->GetNormal();

			if (PO.dot(Pn) < 0.5*dist)
				continue;

			
			// 估计该点如果属于该KeyFrame2应该处于金字塔模型的Level
			int nPredictedLevel = pMP->PredictScale(dist, pKF->m_pPLevelInfo->m_fLogScaleFactor);

			// 判断之前判断的是不是合理，本身代码是有问题的，不能保证求得的Scale Level是合法的
			if (nPredictedLevel >= pKF->m_pPLevelInfo->m_vScaleFactors.size())
				continue;

			// 在前面求得二维投影的KeyFrame某个半径范围内求相关的KeyPoint
			// Level越大意味着图片越小，如果在原图上进行查找的话半径就需要进行相应的放大
			const float radius = th*pKF->m_pPLevelInfo->m_vScaleFactors[nPredictedLevel];

			// // 得到该范围内的KeyPoint的index的集合
			const vector<size_t> vIndices = pKF->GetFeaturesInArea(u, v, radius);

			if (vIndices.empty())
				continue;

			// 和前面的很多方法一样，求与该MapPoint描述符最相近的KeyPoint,同时还满足一定的阈值
			// 就认为该MapPoint和KeyPoint对应的MapPoint匹配上了，当然要跳过已经匹配上的MapPoint
			const cv::Mat dMP = pMP->GetDescriptor();

			int bestDist = 256;
			int bestIdx = -1;
			for (vector<size_t>::const_iterator vit = vIndices.begin(), vend = vIndices.end(); vit != vend; vit++) {
				const size_t idx = *vit;
				if (vpMatched[idx])
					continue;

				const int &kpLevel = pKF->m_vKeysUn[idx].octave;

				if (kpLevel<nPredictedLevel - 1 || kpLevel>nPredictedLevel)
					continue;

				const cv::Mat &dKF = pKF->m_Descriptors.row(idx);

				const int dist = DescriptorDistance(dMP, dKF);

				if (dist < bestDist) {
					bestDist = dist;
					bestIdx = idx;
				}
			}

			if (bestDist <= TH_LOW) {
				vpMatched[bestIdx] = pMP;
				nmatches++;
			}

		}

		return nmatches;
	}

	int ORBmatcher::Fuse(KeyFrame *pKF, cv::Mat Scw, const vector<MapPoint *> &vpPoints, float th, vector<MapPoint *> &vpReplacePoint) {

		// 相机内参
		const float &fx = pKF->m_pCameraInfo->m_fx;
		const float &fy = pKF->m_pCameraInfo->m_fy;
		const float &cx = pKF->m_pCameraInfo->m_cx;
		const float &cy = pKF->m_pCameraInfo->m_cy;

		// 转换成R和t
		cv::Mat sRcw = Scw.rowRange(0, 3).colRange(0, 3);
		const float scw = sqrt(sRcw.row(0).dot(sRcw.row(0)));
		cv::Mat Rcw = sRcw / scw;
		cv::Mat tcw = Scw.rowRange(0, 3).col(3) / scw;
		cv::Mat Ow = -Rcw.t()*tcw;

		// 已经找到的MapPoint，在KeyFrame中所有的，这些MapPoint就不用参与遍历了，
		const set<MapPoint*> spAlreadyFound = pKF->GetMapPoints();

		int nFused = 0;

		const int nPoints = vpPoints.size();

		// For each candidate MapPoint project and match
		for (int iMP = 0; iMP < nPoints; iMP++) {

			MapPoint* pMP = vpPoints[iMP];

			// 忽略已经找到的
			if (pMP->isBad() || spAlreadyFound.count(pMP))
				continue;

			// 得到三维点坐标，准备投影到KeyFrame空间
			cv::Mat p3Dw = pMP->GetWorldPos();

			// 转换到相机坐标系
			cv::Mat p3Dc = Rcw*p3Dw + tcw;

			// depth必须是正数，不然该MapPoint在相机后面是看不到的
			if (p3Dc.at<float>(2) < 0.0)
				continue;

			// 投影到图片上，计算图片的坐标
			const float invz = 1 / p3Dc.at<float>(2);
			const float x = p3Dc.at<float>(0)*invz;
			const float y = p3Dc.at<float>(1)*invz;

			const float u = fx*x + cx;
			const float v = fy*y + cy;

			// 投影到图片上的坐标必须在图片边界范围内
			if (!pKF->IsInImage(u, v))
				continue;

			// 到相机中心的距离还要满足该MapPoint范围限制，不然在该KeyFrame也是找不到该MapPoint的
			const float maxDistance = pMP->GetMaxDistanceInvariance();
			const float minDistance = pMP->GetMinDistanceInvariance();
			cv::Mat PO = p3Dw - Ow;
			const float dist = cv::norm(PO);

			if (dist<minDistance || dist>maxDistance)
				continue;

			// 计算到相机中心的方向和该MapPoint平均方向夹角要小于60度，也就是视角要小于60度
			cv::Mat Pn = pMP->GetNormal();

			if (PO.dot(Pn) < 0.5*dist)
				continue;


			// 估计该点如果属于该KeyFrame2应该处于金字塔模型的Level
			int nPredictedLevel = pMP->PredictScale(dist, pKF->m_pPLevelInfo->m_fLogScaleFactor);

			// 判断之前判断的是不是合理，本身代码是有问题的，不能保证求得的Scale Level是合法的
			if (nPredictedLevel >= pKF->m_pPLevelInfo->m_vScaleFactors.size())
				continue;

			// 在前面求得二维投影的KeyFrame某个半径范围内求相关的KeyPoint
			// Level越大意味着图片越小，如果在原图上进行查找的话半径就需要进行相应的放大
			const float radius = th*pKF->m_pPLevelInfo->m_vScaleFactors[nPredictedLevel];

			// // 得到该范围内的KeyPoint的index的集合
			const vector<size_t> vIndices = pKF->GetFeaturesInArea(u, v, radius);

			if (vIndices.empty())
				continue;

			// 和前面的很多方法一样，求与该MapPoint描述符最相近的KeyPoint,同时还满足一定的阈值
			// 就认为该MapPoint和KeyPoint对应的MapPoint匹配上了，当然要跳过已经匹配上的MapPoint
			const cv::Mat dMP = pMP->GetDescriptor();

			int bestDist = 256;
			int bestIdx = -1;
			for (vector<size_t>::const_iterator vit = vIndices.begin(), vend = vIndices.end(); vit != vend; vit++) {
				const size_t idx = *vit;
				
				const int &kpLevel = pKF->m_vKeysUn[idx].octave;

				if (kpLevel<nPredictedLevel - 1 || kpLevel>nPredictedLevel)
					continue;

				const cv::Mat &dKF = pKF->m_Descriptors.row(idx);

				const int dist = DescriptorDistance(dMP, dKF);

				if (dist < bestDist) {
					bestDist = dist;
					bestIdx = idx;
				}
			}

			if (bestDist <= TH_LOW) {
				MapPoint* pMPinKF = pKF->GetMapPoint(bestIdx); // 在KeyFrame中和该MapPoint匹配上的MapPoint
				if (pMPinKF) {
					if (!pMPinKF->isBad()){
						vpReplacePoint[iMP] = pMPinKF;
						//pMP->Replace(pMPinKF); // 没在这里操作，放在外面，是为了地图的同步，这个操作会使地图中pMP失效
					}
				}
				else {
					pMP->AddObservation(pKF, bestIdx);
					pKF->AddMapPoint(pMP, bestIdx);
				}
				nFused++;
			}
		}

		return nFused;
	}


	void ORBmatcher::ComputeThreeMaxima(vector<int>* histo, const int L, int &ind1, int &ind2, int &ind3) {
		int max1 = 0;
		int max2 = 0;
		int max3 = 0;

		for (int i = 0; i < L; i++) {
			const int s = histo[i].size();
			if (s > max1) {
				max3 = max2;
				max2 = max1;
				max1 = s;
				ind3 = ind2;
				ind2 = ind1;
				ind1 = i;
			} else if (s > max2) {
				max3 = max2;
				max2 = s;
				ind3 = ind2;
				ind2 = i;
			} else if (s > max3) {
				max3 = s;
				ind3 = i;
			}
		}

		if (max2 < 0.1f*(float)max1) {
			ind2 = -1;
			ind3 = -1;
		} else if (max3 < 0.1f*(float)max1) {
			ind3 = -1;
		}
	}


	// Bit set count operation from
	// http://graphics.stanford.edu/~seander/bithacks.html#CountBitsSetParallel
	int ORBmatcher::DescriptorDistance(const cv::Mat &a, const cv::Mat &b) {

		const int *pa = a.ptr<int32_t>();
		const int *pb = b.ptr<int32_t>();

		int dist = 0;

		for (int i = 0; i < 8; i++, pa++, pb++) {
			unsigned  int v = *pa ^ *pb;
			v = v - ((v >> 1) & 0x55555555);
			v = (v & 0x33333333) + ((v >> 2) & 0x33333333);
			dist += (((v + (v >> 4)) & 0xF0F0F0F) * 0x1010101) >> 24;
		}

		return dist;
	}
	
	bool ORBmatcher::CheckDistEpipolarLine(const cv::KeyPoint &kp1, const cv::KeyPoint &kp2, const cv::Mat &F12, const KeyFrame* pKF2) {
		// Epipolar line in second image l = x1'F12 = [a b c]
		const float a = kp1.pt.x*F12.at<float>(0, 0) + kp1.pt.y*F12.at<float>(1, 0) + F12.at<float>(2, 0);
		const float b = kp1.pt.x*F12.at<float>(0, 1) + kp1.pt.y*F12.at<float>(1, 1) + F12.at<float>(2, 1);
		const float c = kp1.pt.x*F12.at<float>(0, 2) + kp1.pt.y*F12.at<float>(1, 2) + F12.at<float>(2, 2);

		const float num = a*kp2.pt.x + b*kp2.pt.y + c;

		const float den = a*a + b*b;

		if (den == 0)
			return false;

		const float dsqr = num*num / den;

		return dsqr < 3.84*pKF2->m_pPLevelInfo->m_vLevelSigma2[kp2.octave];  // 经验值？？？？？
	}
}