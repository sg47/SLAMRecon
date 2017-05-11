// Copyright 2014-2015 Isis Innovation Limited and the authors of InfiniTAM
#ifndef _FE_RENDERSTATE_H
#define _FE_RENDERSTATE_H

#include <stdlib.h>

#include "../Utils/FELibDefines.h"

namespace FE
{
	/** \brief
		Stores the render state used by the SceneReconstruction
		and Visualisation engines.
		*/
	class FERenderState
	{
	public:
		/** @brief
		Gives the raycasting operations an idea of the
		depth range to cover

		Each pixel contains an expected minimum and maximum
		depth. The raycasting step would use this
		information to reduce the range for searching an
		intersection with the actual surface. Should be
		updated by a FE::FETMVisualisationEngine
		before any raycasting operation.
		*/
		Basis::Image<Vector2f> *renderingRangeImage;

		/** @brief
		Float rendering output of the scene, containing the 3D
		locations in the world generated by the raycast.

		This is typically created as a by-product of
		raycasting operations.
		*/
		Basis::Image<Vector4f> *raycastResult;

		Basis::Image<Vector4f> *forwardProjection;
		Basis::Image<int> *fwdProjMissingPoints;
		int noFwdProjMissingPoints;

		Basis::Image<Vector4u> *raycastImage;

		FERenderState(const Vector2i &imgSize, float vf_min, float vf_max, MemoryDeviceType memoryType)
		{
			renderingRangeImage = new Basis::Image<Vector2f>(imgSize, memoryType);
			raycastResult = new Basis::Image<Vector4f>(imgSize, memoryType);
			forwardProjection = new Basis::Image<Vector4f>(imgSize, memoryType);
			fwdProjMissingPoints = new Basis::Image<int>(imgSize, memoryType);
			raycastImage = new Basis::Image<Vector4u>(imgSize, memoryType);

			Basis::Image<Vector2f> *buffImage = new Basis::Image<Vector2f>(imgSize, MEMORYDEVICE_CPU);

			Vector2f v_lims(vf_min, vf_max);
			for (int i = 0; i < imgSize.x * imgSize.y; i++) buffImage->GetData(MEMORYDEVICE_CPU)[i] = v_lims;

			if (memoryType == MEMORYDEVICE_CUDA)
			{
				renderingRangeImage->SetFrom(buffImage, Basis::MemoryBlock<Vector2f>::CPU_TO_CUDA);
			}
			else renderingRangeImage->SetFrom(buffImage, Basis::MemoryBlock<Vector2f>::CPU_TO_CPU);

			delete buffImage;

			noFwdProjMissingPoints = 0;
		}

		virtual ~FERenderState()
		{
			delete renderingRangeImage;
			delete raycastResult;
			delete forwardProjection;
			delete fwdProjMissingPoints;
			delete raycastImage;
		}

		void setRenderingRangeImage(const Vector2i &imgSize, const float vf_min, const float vf_max, MemoryDeviceType memoryType){
			Basis::Image<Vector2f> *buffImage = new Basis::Image<Vector2f>(imgSize, MEMORYDEVICE_CPU);

			Vector2f v_lims(vf_min, vf_max);
			for (int i = 0; i < imgSize.x * imgSize.y; i++) buffImage->GetData(MEMORYDEVICE_CPU)[i] = v_lims;

			if (memoryType == MEMORYDEVICE_CUDA)
			{
				renderingRangeImage->SetFrom(buffImage, Basis::MemoryBlock<Vector2f>::CPU_TO_CUDA);
			}
			else renderingRangeImage->SetFrom(buffImage, Basis::MemoryBlock<Vector2f>::CPU_TO_CPU);

			delete buffImage;
		}
	};
}

#endif //_FE_RENDERSTATE_H
