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

#include "precompiled.h"

#include "LOSTexture.h"

#include "graphics/ShaderManager.h"
#include "lib/bits.h"
#include "lib/config2.h"
#include "ps/CLogger.h"
#include "ps/CStrInternStatic.h"
#include "ps/Game.h"
#include "ps/Profile.h"
#include "renderer/Renderer.h"
#include "renderer/RenderingOptions.h"
#include "renderer/TimeManager.h"
#include "simulation2/Simulation2.h"
#include "simulation2/components/ICmpRangeManager.h"
#include "simulation2/helpers/Los.h"

/*

The LOS bitmap is computed with one value per LOS vertex, based on
CCmpRangeManager's visibility information.

The bitmap is then blurred using an NxN filter (in particular a
7-tap Binomial filter as an efficient integral approximation of a Gaussian).
To implement the blur efficiently without using extra memory for a second copy
of the bitmap, we generate the bitmap with (N-1)/2 pixels of padding on each side,
then the blur shifts the image back into the corner.

The blurred bitmap is then uploaded into a GL texture for use by the renderer.

*/


// Blur with a NxN filter, where N = g_BlurSize must be an odd number.
// Keep it in relation to the number of impassable tiles in MAP_EDGE_TILES.
static const size_t g_BlurSize = 7;

// Alignment (in bytes) of the pixel data passed into texture uploading.
// This must be a multiple of GL_UNPACK_ALIGNMENT, which ought to be 1 (since
// that's what we set it to) but in some weird cases appears to have a different
// value. (See Trac #2594). Multiples of 4 are possibly good for performance anyway.
static const size_t g_SubTextureAlignment = 4;

CLOSTexture::CLOSTexture(CSimulation2& simulation)
	: m_Simulation(simulation)
{
	if (CRenderer::IsInitialised() && g_RenderingOptions.GetSmoothLOS())
		CreateShader();
}

CLOSTexture::~CLOSTexture()
{
	if (m_SmoothFBO1)
		glDeleteFramebuffersEXT(1, &m_SmoothFBO1);
	if (m_SmoothFBO2)
		glDeleteFramebuffersEXT(1, &m_SmoothFBO2);

	if (m_Texture)
		DeleteTexture();
}

// Create the LOS texture engine. Should be ran only once.
bool CLOSTexture::CreateShader()
{
	m_SmoothTech = g_Renderer.GetShaderManager().LoadEffect(str_los_interp);
	CShaderProgramPtr shader = m_SmoothTech->GetShader();

	m_ShaderInitialized = m_SmoothTech && shader;

	if (!m_ShaderInitialized)
	{
		LOGERROR("Failed to load SmoothLOS shader, disabling.");
		g_RenderingOptions.SetSmoothLOS(false);
		return false;
	}

	glGenFramebuffersEXT(1, &m_SmoothFBO1);
	glGenFramebuffersEXT(1, &m_SmoothFBO2);
	return true;
}

void CLOSTexture::DeleteTexture()
{
	m_Texture.reset();
	m_TextureSmooth1.reset();
	m_TextureSmooth2.reset();
}

void CLOSTexture::MakeDirty()
{
	m_Dirty = true;
}

Renderer::Backend::GL::CTexture* CLOSTexture::GetTextureSmooth()
{
	if (CRenderer::IsInitialised() && !g_RenderingOptions.GetSmoothLOS())
		return GetTexture();
	else
		return (m_WhichTex ? m_TextureSmooth1 : m_TextureSmooth2).get();
}

void CLOSTexture::InterpolateLOS(Renderer::Backend::GL::CDeviceCommandContext* deviceCommandContext)
{
	const bool skipSmoothLOS = CRenderer::IsInitialised() && !g_RenderingOptions.GetSmoothLOS();
	if (!skipSmoothLOS && !m_ShaderInitialized)
	{
		if (!CreateShader())
			return;

		// RecomputeTexture will not cause the ConstructTexture to run.
		// Force the textures to be created.
		DeleteTexture();
		ConstructTexture(deviceCommandContext);
		m_Dirty = true;
	}

	if (m_Dirty)
	{
		RecomputeTexture(deviceCommandContext);
		m_Dirty = false;
	}

	if (skipSmoothLOS)
		return;

	glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, (m_WhichTex ? m_SmoothFBO2 : m_SmoothFBO1));

	m_SmoothTech->BeginPass();
	deviceCommandContext->SetGraphicsPipelineState(
		m_SmoothTech->GetGraphicsPipelineStateDesc());

	const CShaderProgramPtr& shader = m_SmoothTech->GetShader();

	shader->BindTexture(str_losTex1, m_Texture.get());
	shader->BindTexture(str_losTex2, (m_WhichTex ? m_TextureSmooth1 : m_TextureSmooth2).get());

	shader->Uniform(str_delta, (float)g_Renderer.GetTimeManager().GetFrameDelta() * 4.0f, 0.0f, 0.0f, 0.0f);

	const SViewPort oldVp = g_Renderer.GetViewport();
	const SViewPort vp =
	{
		0, 0,
		static_cast<int>(m_Texture->GetWidth()),
		static_cast<int>(m_Texture->GetHeight())
	};
	g_Renderer.SetViewport(vp);

	float quadVerts[] =
	{
		1.0f, 1.0f,
		-1.0f, 1.0f,
		-1.0f, -1.0f,

		-1.0f, -1.0f,
		1.0f, -1.0f,
		1.0f, 1.0f
	};
	float quadTex[] =
	{
		1.0f, 1.0f,
		0.0f, 1.0f,
		0.0f, 0.0f,

		0.0f, 0.0f,
		1.0f, 0.0f,
		1.0f, 1.0f
	};
	shader->TexCoordPointer(GL_TEXTURE0, 2, GL_FLOAT, 0, quadTex);
	shader->VertexPointer(2, GL_FLOAT, 0, quadVerts);
	shader->AssertPointersBound();
	glDrawArrays(GL_TRIANGLES, 0, 6);

	g_Renderer.SetViewport(oldVp);

	m_SmoothTech->EndPass();

	glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);

	m_WhichTex = !m_WhichTex;
}


Renderer::Backend::GL::CTexture* CLOSTexture::GetTexture()
{
	ENSURE(!m_Dirty);
	return m_Texture.get();
}

const CMatrix3D& CLOSTexture::GetTextureMatrix()
{
	ENSURE(!m_Dirty);
	return m_TextureMatrix;
}

const CMatrix3D& CLOSTexture::GetMinimapTextureMatrix()
{
	ENSURE(!m_Dirty);
	return m_MinimapTextureMatrix;
}

void CLOSTexture::ConstructTexture(Renderer::Backend::GL::CDeviceCommandContext* deviceCommandContext)
{
	CmpPtr<ICmpRangeManager> cmpRangeManager(m_Simulation, SYSTEM_ENTITY);
	if (!cmpRangeManager)
		return;

	m_MapSize = cmpRangeManager->GetVerticesPerSide();

	const size_t textureSize = round_up_to_pow2(round_up((size_t)m_MapSize + g_BlurSize - 1, g_SubTextureAlignment));

	const Renderer::Backend::Sampler::Desc defaultSamplerDesc =
		Renderer::Backend::Sampler::MakeDefaultSampler(
			Renderer::Backend::Sampler::Filter::LINEAR,
			Renderer::Backend::Sampler::AddressMode::CLAMP_TO_EDGE);

	m_Texture = Renderer::Backend::GL::CTexture::Create2D(
		Renderer::Backend::Format::A8, textureSize, textureSize, defaultSamplerDesc);

	// Initialise texture with SoD color, for the areas we don't
	// overwrite with uploading later.
	std::unique_ptr<u8[]> texData = std::make_unique<u8[]>(textureSize * textureSize);
	memset(texData.get(), 0x00, textureSize * textureSize);

	if (CRenderer::IsInitialised() && g_RenderingOptions.GetSmoothLOS())
	{
		m_TextureSmooth1 = Renderer::Backend::GL::CTexture::Create2D(
			Renderer::Backend::Format::A8, textureSize, textureSize, defaultSamplerDesc);
		m_TextureSmooth2 = Renderer::Backend::GL::CTexture::Create2D(
			Renderer::Backend::Format::A8, textureSize, textureSize, defaultSamplerDesc);

		glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, m_SmoothFBO1);
		glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D,
			m_TextureSmooth1->GetHandle(), 0);
		GLenum status1 = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
		glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, m_SmoothFBO2);
		glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D,
			m_TextureSmooth2->GetHandle(), 0);
		GLenum status2 = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
		if (status1 != GL_FRAMEBUFFER_COMPLETE_EXT || status2 != GL_FRAMEBUFFER_COMPLETE_EXT)
		{
			LOGWARNING("LOS framebuffer object incomplete: 0x%04X 0x%04X", status1, status2);
		}
		glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);

		deviceCommandContext->UploadTexture(m_TextureSmooth1.get(), Renderer::Backend::Format::A8, texData.get(), textureSize * textureSize);
		deviceCommandContext->UploadTexture(m_TextureSmooth2.get(), Renderer::Backend::Format::A8, texData.get(), textureSize * textureSize);
	}

	deviceCommandContext->UploadTexture(m_Texture.get(), Renderer::Backend::Format::A8, texData.get(), textureSize * textureSize);

	texData.reset();

	{
		// Texture matrix: We want to map
		//   world pos (0, y, 0)  (i.e. first vertex)
		//     onto texcoord (0.5/texsize, 0.5/texsize)  (i.e. middle of first texel);
		//   world pos ((mapsize-1)*cellsize, y, (mapsize-1)*cellsize)  (i.e. last vertex)
		//     onto texcoord ((mapsize-0.5) / texsize, (mapsize-0.5) / texsize)  (i.e. middle of last texel)

		float s = (m_MapSize-1) / static_cast<float>(textureSize * (m_MapSize-1) * LOS_TILE_SIZE);
		float t = 0.5f / textureSize;
		m_TextureMatrix.SetZero();
		m_TextureMatrix._11 = s;
		m_TextureMatrix._23 = s;
		m_TextureMatrix._14 = t;
		m_TextureMatrix._24 = t;
		m_TextureMatrix._44 = 1;
	}

	{
		// Minimap matrix: We want to map UV (0,0)-(1,1) onto (0,0)-(mapsize/texsize, mapsize/texsize)

		float s = m_MapSize / (float)textureSize;
		m_MinimapTextureMatrix.SetZero();
		m_MinimapTextureMatrix._11 = s;
		m_MinimapTextureMatrix._22 = s;
		m_MinimapTextureMatrix._44 = 1;
	}
}

void CLOSTexture::RecomputeTexture(Renderer::Backend::GL::CDeviceCommandContext* deviceCommandContext)
{
	// If the map was resized, delete and regenerate the texture
	if (m_Texture)
	{
		CmpPtr<ICmpRangeManager> cmpRangeManager(m_Simulation, SYSTEM_ENTITY);
		if (!cmpRangeManager || m_MapSize != cmpRangeManager->GetVerticesPerSide())
			DeleteTexture();
	}

	bool recreated = false;
	if (!m_Texture)
	{
		ConstructTexture(deviceCommandContext);
		recreated = true;
	}

	PROFILE("recompute LOS texture");

	size_t pitch;
	const size_t dataSize = GetBitmapSize(m_MapSize, m_MapSize, &pitch);
	ENSURE(pitch * m_MapSize <= dataSize);
	std::unique_ptr<u8[]> losData = std::make_unique<u8[]>(dataSize);

	CmpPtr<ICmpRangeManager> cmpRangeManager(m_Simulation, SYSTEM_ENTITY);
	if (!cmpRangeManager)
		return;

	CLosQuerier los(cmpRangeManager->GetLosQuerier(g_Game->GetSimulation2()->GetSimContext().GetCurrentDisplayedPlayer()));

	GenerateBitmap(los, &losData[0], m_MapSize, m_MapSize, pitch);

	if (CRenderer::IsInitialised() && g_RenderingOptions.GetSmoothLOS() && recreated)
	{
		deviceCommandContext->UploadTextureRegion(
			m_TextureSmooth1.get(), Renderer::Backend::Format::A8, losData.get(),
			pitch * m_MapSize, 0, 0, pitch, m_MapSize);
		deviceCommandContext->UploadTextureRegion(
			m_TextureSmooth2.get(), Renderer::Backend::Format::A8, losData.get(),
			pitch * m_MapSize, 0, 0, pitch, m_MapSize);
	}

	deviceCommandContext->UploadTextureRegion(
		m_Texture.get(), Renderer::Backend::Format::A8, losData.get(),
		pitch * m_MapSize, 0, 0, pitch, m_MapSize);
}

size_t CLOSTexture::GetBitmapSize(size_t w, size_t h, size_t* pitch)
{
	*pitch = round_up(w + g_BlurSize - 1, g_SubTextureAlignment);
	return *pitch * (h + g_BlurSize - 1);
}

void CLOSTexture::GenerateBitmap(const CLosQuerier& los, u8* losData, size_t w, size_t h, size_t pitch)
{
	u8 *dataPtr = losData;

	// Initialise the top padding
	for (size_t j = 0; j < g_BlurSize/2; ++j)
		for (size_t i = 0; i < pitch; ++i)
			*dataPtr++ = 0;

	for (size_t j = 0; j < h; ++j)
	{
		// Initialise the left padding
		for (size_t i = 0; i < g_BlurSize/2; ++i)
			*dataPtr++ = 0;

		// Fill in the visibility data
		for (size_t i = 0; i < w; ++i)
		{
			if (los.IsVisible_UncheckedRange(i, j))
				*dataPtr++ = 255;
			else if (los.IsExplored_UncheckedRange(i, j))
				*dataPtr++ = 127;
			else
				*dataPtr++ = 0;
		}

		// Initialise the right padding
		for (size_t i = 0; i < pitch - w - g_BlurSize/2; ++i)
			*dataPtr++ = 0;
	}

	// Initialise the bottom padding
	for (size_t j = 0; j < g_BlurSize/2; ++j)
		for (size_t i = 0; i < pitch; ++i)
			*dataPtr++ = 0;

	// Horizontal blur:

	for (size_t j = g_BlurSize/2; j < h + g_BlurSize/2; ++j)
	{
		for (size_t i = 0; i < w; ++i)
		{
			u8* d = &losData[i+j*pitch];
			*d = (
				1*d[0] +
				6*d[1] +
				15*d[2] +
				20*d[3] +
				15*d[4] +
				6*d[5] +
				1*d[6]
			) / 64;
		}
	}

	// Vertical blur:

	for (size_t j = 0; j < h; ++j)
	{
		for (size_t i = 0; i < w; ++i)
		{
			u8* d = &losData[i+j*pitch];
			*d = (
				1*d[0*pitch] +
				6*d[1*pitch] +
				15*d[2*pitch] +
				20*d[3*pitch] +
				15*d[4*pitch] +
				6*d[5*pitch] +
				1*d[6*pitch]
			) / 64;
		}
	}
}
