/* Copyright (C) 2022 Wildfire Games.
 * This file is part of 0 A.D.
 *
 * 0 A.D. is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * 0 A.D. is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with 0 A.D.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef INCLUDED_LOSTEXTURE
#define INCLUDED_LOSTEXTURE

#include "lib/ogl.h"

#include "graphics/ShaderTechniquePtr.h"
#include "maths/Matrix3D.h"
#include "renderer/backend/gl/DeviceCommandContext.h"
#include "renderer/backend/gl/Texture.h"

#include <memory>

class CLosQuerier;
class CSimulation2;

/**
 * Maintains the LOS (fog-of-war / shroud-of-darkness) texture, used for
 * rendering and for the minimap.
 */
class CLOSTexture
{
	NONCOPYABLE(CLOSTexture);
	friend class TestLOSTexture;

public:
	CLOSTexture(CSimulation2& simulation);
	~CLOSTexture();

	/**
	 * Marks the LOS texture as needing recomputation. Call this after each
	 * simulation update, to ensure responsive updates.
	 */
	void MakeDirty();

	/**
	 * Recomputes the LOS texture if necessary, and returns the texture handle.
	 * Also potentially switches the current active texture unit, and enables texturing on it.
	 * The texture is in 8-bit ALPHA format.
	 */
	Renderer::Backend::GL::CTexture* GetTexture();
	Renderer::Backend::GL::CTexture* GetTextureSmooth();

	void InterpolateLOS(Renderer::Backend::GL::CDeviceCommandContext* deviceCommandContext);

	/**
	 * Returns a matrix to map (x,y,z) world coordinates onto (u,v) LOS texture
	 * coordinates, in the form expected by a matrix uniform.
	 * This must only be called after InterpolateLOS.
	 */
	const CMatrix3D& GetTextureMatrix();

	/**
	 * Returns a matrix to map (0,0)-(1,1) texture coordinates onto LOS texture
	 * coordinates, in the form expected by a matrix uniform.
	 * This must only be called after InterpolateLOS.
	 */
	const CMatrix3D& GetMinimapTextureMatrix();

private:
	void DeleteTexture();
	bool CreateShader();
	void ConstructTexture(Renderer::Backend::GL::CDeviceCommandContext* deviceCommandContext);
	void RecomputeTexture(Renderer::Backend::GL::CDeviceCommandContext* deviceCommandContext);

	size_t GetBitmapSize(size_t w, size_t h, size_t* pitch);
	void GenerateBitmap(const CLosQuerier& los, u8* losData, size_t w, size_t h, size_t pitch);

	CSimulation2& m_Simulation;

	bool m_Dirty = true;

	bool m_ShaderInitialized = false;

	std::unique_ptr<Renderer::Backend::GL::CTexture>
		m_Texture, m_TextureSmooth1, m_TextureSmooth2;

	bool m_WhichTex = true;

	// We update textures once a frame, so we change a FBO once a frame. That
	// allows us to use two ping-pong FBOs instead of checking completeness of
	// FBO each frame.
	GLuint m_SmoothFBO1 = 0, m_SmoothFBO2 = 0;
	CShaderTechniquePtr m_SmoothTech;

	size_t m_MapSize = 0; // vertexes per side

	CMatrix3D m_TextureMatrix;
	CMatrix3D m_MinimapTextureMatrix;
};

#endif // INCLUDED_LOSTEXTURE
