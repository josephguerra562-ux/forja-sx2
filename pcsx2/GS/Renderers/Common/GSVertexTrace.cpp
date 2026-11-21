// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GSVertexTrace.h"
#include "GS/GSUtil.h"
#include "GS/GSState.h"

#include "common/Console.h"

#include <atomic>
#include <cstring>

// 171 in the app, "Vértices una vez" ([Forja] GSVerticesUnaVez, set at the game's start by ps4/vm/main.cpp). FindMinMax goes through the
// draw's index list, so a vertex shared by 3 triangles (a strip made into a list) was read 3 times: 9.3 % of the GS thread in God of War
// II's Colossus. For triangles with smooth shading (IIP: every vertex's color counts, and each one's own Q divides its S and T) the
// minimum and maximum are the same reading each vertex once; VertexKick keeps the vertices a draw uses at the front of its buffer
// (a strip's skipped triangles are copied over, a list's are dropped), so they are 0 .. the highest index. Fans keep the old way.
// Mode: 0 off, 1 check (both ways, compared; the old one's result is kept), 2 on. 1 goes to 2 by itself after 20,000 draws the same,
// and one different turns it off (g_forja_gs_vt_once_first_wrong says which).
std::atomic<int> g_forja_gs_vt_once{0};
std::atomic<bool> g_forja_gs_vt_once_check_only{false};
std::atomic<u64> g_forja_gs_vt_once_checked{0}, g_forja_gs_vt_once_wrong{0}, g_forja_gs_vt_once_draws{0}, g_forja_gs_vt_once_saved{0};
u32 g_forja_gs_vt_once_first_wrong[4] = {}; // indices, vertices, which part (1 p, 2 t, 3 c, 4 nan), prim

namespace
{
	// 172 in the app: the vertices a draw's indices use, each once. The 171 took 0 .. the highest index, and its check found a God of War II
	// strip (549 indices, 353 vertices) where that took in vertices no triangle uses. 173: a strip's triangles come from VertexKick as
	// 3 consecutive slots, each one slot on from the last, or right after the last one's slots when skipped triangles were copied over;
	// so a triangle's new vertices are the ones above the highest of the triangles before it. Only vertices the indices name go in, and
	// the 172's bitmap (~2.7 % of the GS: a read-modify-write per index, the same word three times in a row) is gone. A vertex this left
	// out could only change a minimum or maximum the check would see.
	alignas(16) u16 s_forja_used[65536];

	u32 ForjaUsedVertices(const u16* index, int count, int v_count)
	{
		(void)v_count;
		int highest = -1;
		u32 used = 0;
		for (int i = 0; i + 2 < count; i += 3)
		{
			const int x = index[i], y = index[i + 1], z = index[i + 2];
			if (x > highest)
				s_forja_used[used++] = static_cast<u16>(x);
			if (y > highest)
				s_forja_used[used++] = static_cast<u16>(y);
			if (z > highest)
				s_forja_used[used++] = static_cast<u16>(z);
			highest = std::max(highest, std::max(x, std::max(y, z)));
		}
		return used;
	}

	bool ForjaSameFloats(const GSVector4& a, const GSVector4& b)
	{
		return (a == b).mask() == 0xf; // +0 and -0 count as the same: which one a min keeps depends on the order
	}
} // namespace

GSVertexTrace::GSVertexTrace(const GSState* state)
	: m_state(state)
{
	MULTI_ISA_SELECT(GSVertexTracePopulateFunctions)(*this);
}

void GSVertexTrace::Update(const void* vertex, const u16* index, int v_count, int i_count, GS_PRIM_CLASS primclass)
{
	if (i_count == 0)
		return;

	m_primclass = primclass;

	const u32 iip = m_state->PRIM->IIP;
	const u32 tme = m_state->PRIM->TME;
	const u32 fst = m_state->PRIM->FST;
	const u32 color = !(m_state->PRIM->TME && m_state->m_context->TEX0.TFX == TFX_DECAL && m_state->m_context->TEX0.TCC);

	// 171 in the app, "Vértices una vez".
	bool forja_done = false;
	const int forja_once = g_forja_gs_vt_once.load(std::memory_order_relaxed);
	// 172 in the app: only strips (a list's vertices are used once each anyway; fans keep the old way), from the bitmap of used vertices.
	if (forja_once && iip && primclass == GS_TRIANGLE_CLASS && m_state->PRIM->PRIM == GS_TRIANGLESTRIP && i_count >= 6 &&
		v_count > 0 && v_count <= 65536)
	{
		const u32 unique = ForjaUsedVertices(index, i_count, v_count);
		if (unique * 3 <= static_cast<u32>(i_count) * 2)
		{
			if (forja_once == 2)
			{
				m_fmm[color][fst][tme][iip][primclass](*this, vertex, s_forja_used, static_cast<int>(unique));
				g_forja_gs_vt_once_draws.fetch_add(1, std::memory_order_relaxed);
				g_forja_gs_vt_once_saved.fetch_add(static_cast<u32>(i_count) - unique, std::memory_order_relaxed);
			}
			else
			{
				m_fmm[color][fst][tme][iip][primclass](*this, vertex, index, i_count);
				const Vertex old_min = m_min, old_max = m_max;
				const u32 old_nan = nan.value;
				m_fmm[color][fst][tme][iip][primclass](*this, vertex, s_forja_used, static_cast<int>(unique));
				const u32 which = !ForjaSameFloats(old_min.p, m_min.p) || !ForjaSameFloats(old_max.p, m_max.p) ? 1 :
								  !ForjaSameFloats(old_min.t, m_min.t) || !ForjaSameFloats(old_max.t, m_max.t) ? 2 :
								  !old_min.c.eq(m_min.c) || !old_max.c.eq(m_max.c)                           ? 3 :
								  old_nan != nan.value                                                       ? 4 : 0;
				m_min = old_min;
				m_max = old_max;
				nan.value = old_nan;
				if (which == 0)
				{
					const u64 ok = g_forja_gs_vt_once_checked.fetch_add(1, std::memory_order_relaxed) + 1;
					if (ok >= 20000 && !g_forja_gs_vt_once_check_only.load(std::memory_order_relaxed) &&
						g_forja_gs_vt_once_wrong.load(std::memory_order_relaxed) == 0)
						g_forja_gs_vt_once.store(2, std::memory_order_relaxed);
				}
				else
				{
					if (g_forja_gs_vt_once_wrong.load(std::memory_order_relaxed) == 0)
					{
						g_forja_gs_vt_once_first_wrong[0] = static_cast<u32>(i_count);
						g_forja_gs_vt_once_first_wrong[1] = unique;
						g_forja_gs_vt_once_first_wrong[2] = which;
						g_forja_gs_vt_once_first_wrong[3] = m_state->PRIM->PRIM;
					}
					g_forja_gs_vt_once_wrong.fetch_add(1, std::memory_order_release);
					g_forja_gs_vt_once.store(0, std::memory_order_relaxed);
				}
			}
			forja_done = true;
		}
	}
	if (!forja_done)
		m_fmm[color][fst][tme][iip][primclass](*this, vertex, index, i_count);

	// Potential float overflow detected. Better uses the slower division instead
	// Note: If Q is too big, 1/Q will end up as 0. 1e30 is a random number
	// that feel big enough.
	if (!fst && !m_accurate_stq && m_min.t.z > 1e30)
	{
		Console.Warning("Vertex Trace: float overflow detected ! min %e max %e", m_min.t.z, m_max.t.z);
		m_accurate_stq = true;
	}

	// AA1: Set alpha min max to coverage 128 when there is no alpha blending.
	if (!m_state->PRIM->ABE && m_state->PRIM->AA1 && (m_primclass == GS_LINE_CLASS || m_primclass == GS_TRIANGLE_CLASS))
	{
		m_min.c.a = 128;
		m_max.c.a = 128;
	}

	m_eq.value = (m_min.c == m_max.c).mask() | ((m_min.p == m_max.p).mask() << 16) | ((m_min.t == m_max.t).mask() << 20);

	m_alpha.valid = false;

	// I'm not sure of the cost. In doubt let's do it only when depth is enabled
	if (m_state->m_context->TEST.ZTE == 1 && m_state->m_context->TEST.ZTST > ZTST_ALWAYS)
	{
		CorrectDepthTrace(vertex, v_count);
	}

	if (tme)
	{
		const GIFRegTEX1& TEX1 = m_state->m_context->TEX1;

		m_filter.mmag = TEX1.IsMagLinear();
		m_filter.mmin = TEX1.IsMinLinear();

		if (TEX1.MXL == 0) // MXL == 0 => MMIN ignored, tested it on ps2
		{
			m_filter.linear = m_filter.mmag;
		}
		else
		{
			const float K = static_cast<float>(TEX1.K) / 16;

			if (TEX1.LCM == 0 && m_state->PRIM->FST == 0) // FST == 1 => Q is not interpolated
			{
				// LOD = log2(1/|Q|) * (1 << L) + K

				GSVector4::storel(&m_lod, m_max.t.uph(m_min.t).log2(3).neg() * static_cast<float>(1 << TEX1.L) + K);

				if (m_lod.x > m_lod.y)
				{
					const float tmp = m_lod.x;
					m_lod.x = m_lod.y;
					m_lod.y = tmp;
				}
			}
			else
			{
				m_lod.x = K;
				m_lod.y = K;
			}

			if (m_lod.y <= 0)
			{
				m_filter.linear = m_filter.mmag;
			}
			else if (m_lod.x > 0)
			{
				m_filter.linear = m_filter.mmin;
			}
			else
			{
				m_filter.linear = m_filter.mmag | m_filter.mmin;
			}
		}

		switch (GSConfig.TextureFiltering)
		{
			case BiFiltering::Nearest:
				m_filter.opt_linear = 0;
				break;

			case BiFiltering::Forced_But_Sprite:
				// Special case to reduce the number of glitch when upscaling is enabled
				m_filter.opt_linear = (m_primclass == GS_SPRITE_CLASS) ? m_filter.linear : 1;
				break;

			case BiFiltering::Forced:
				m_filter.opt_linear = 1;
				break;

			case BiFiltering::PS2:
			default:
				m_filter.opt_linear = m_filter.linear;
				break;
		}
	}
}

void GSVertexTrace::CorrectDepthTrace(const void* vertex, int count)
{
	if (m_eq.z == 0)
		return;

	// FindMinMax isn't accurate for the depth value. Lsb bit is always 0.
	// The code below will check that depth value is really constant
	// and will update m_min/m_max/m_eq accordingly
	//
	// Really impact Xenosaga3
	//
	// Hopefully function is barely called so AVX/SSE will be useless here


	const GSVertex* RESTRICT v = (GSVertex*)vertex;

	const int sprite_step = (m_primclass == GS_SPRITE_CLASS) ? 1 : 0;

	u32 z = v[sprite_step].XYZ.Z;

	if (z & 1)
	{
		// Check that first bit is always 1
		for (int i = sprite_step; i < count; i += (sprite_step + 1))
		{
			z &= v[i].XYZ.Z;
		}
	}
	else
	{
		// Check that first bit is always 0
		for (int i = sprite_step; i < count; i += (sprite_step + 1))
		{
			z |= v[i].XYZ.Z;
		}
	}

	if (z == v[sprite_step].XYZ.Z)
	{
		m_eq.z = 1;
	}
	else
	{
		m_eq.z = 0;
	}
}
