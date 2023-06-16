#include "stdafx.h"

#include "IGame_Persistent.h"
#include "environment.h"
#include "x_ray.h"
#include "IGame_Level.h"
#include "XR_IOConsole.h"
#include "Render.h"
#include "ps_instance.h"
#include "CustomHUD.h"

ENGINE_API IGame_Persistent* g_pGamePersistent = NULL;

ENGINE_API bool IsMainMenuActive() { return g_pGamePersistent && g_pGamePersistent->m_pMainMenu && g_pGamePersistent->m_pMainMenu->IsActive(); }

ENGINE_API BOOL g_prefetch{TRUE};

IGame_Persistent::IGame_Persistent()
{
    Device.seqAppStart.Add(this);
    Device.seqAppEnd.Add(this);
    Device.seqFrame.Add(this, REG_PRIORITY_HIGH + 1);
    Device.seqAppActivate.Add(this);
    Device.seqAppDeactivate.Add(this);

    m_pMainMenu = NULL;

    pEnvironment = xr_new<CEnvironment>();

    m_pGShaderConstants = ShadersExternalData(); //--#SM+#--
}

IGame_Persistent::~IGame_Persistent()
{
    Device.seqFrame.Remove(this);
    Device.seqAppStart.Remove(this);
    Device.seqAppEnd.Remove(this);
    Device.seqAppActivate.Remove(this);
    Device.seqAppDeactivate.Remove(this);
    xr_delete(pEnvironment);
}

void IGame_Persistent::OnAppActivate() {}

void IGame_Persistent::OnAppDeactivate() {}

void IGame_Persistent::OnAppStart() { Environment().load(); }

void IGame_Persistent::OnAppEnd()
{
    Environment().unload();
    OnGameEnd();

    DEL_INSTANCE(g_hud);
}

void IGame_Persistent::PreStart(LPCSTR op)
{
    string256 prev_type;
    params new_game_params;
    strcpy_s(prev_type, m_game_params.m_game_type);
    new_game_params.parse_cmd_line(op);

    // change game type
    if (0 != xr_strcmp(prev_type, new_game_params.m_game_type))
    {
        OnGameEnd();
    }
}
void IGame_Persistent::Start(LPCSTR op)
{
    string256 prev_type;
    strcpy_s(prev_type, m_game_params.m_game_type);
    m_game_params.parse_cmd_line(op);
    // change game type
    if ((0 != xr_strcmp(prev_type, m_game_params.m_game_type)))
    {
        if (*m_game_params.m_game_type)
            OnGameStart();
        if (g_hud)
            DEL_INSTANCE(g_hud);
    }
    else
        UpdateGameType();

    VERIFY(ps_destroy.empty());
}

void IGame_Persistent::Disconnect()
{
    // clear "need to play" particles
    destroy_particles(true);

    if (g_hud)
        g_hud->OnDisconnected();

    if (!g_prefetch) // очистка при выходе из игры в главное меню
    {
        ObjectPool.clear();
    }
}

void IGame_Persistent::OnGameStart()
{
    LoadTitle("st_prefetching_objects");

    if (!g_prefetch)
        return;

    // prefetch game objects & models
    float p_time = 1000.f * Device.GetTimerGlobal()->GetElapsed_sec();
    u32 mem_0 = Memory.mem_usage();

    Log("Loading objects...");
    ObjectPool.prefetch();
    Log("Loading models...");
    Render->models_Prefetch();

    p_time = 1000.f * Device.GetTimerGlobal()->GetElapsed_sec() - p_time;
    u32 p_mem = Memory.mem_usage() - mem_0;

    Msg("* [prefetch] time:    %d ms", iFloor(p_time));
    Msg("* [prefetch] memory:  %dKb", p_mem / 1024);
}

void IGame_Persistent::OnGameEnd()
{
    ObjectPool.clear();
    Render->models_Clear(TRUE);
}

void IGame_Persistent::OnFrame()
{
    if (!Device.Paused() || Device.dwPrecacheFrame)
        Environment().OnFrame();

    Device.Statistic->Particles_starting = ps_needtoplay.size();
    Device.Statistic->Particles_active = ps_active.size();
    Device.Statistic->Particles_destroy = ps_destroy.size();

    // Play req particle systems
    while (!ps_needtoplay.empty())
    {
        auto& psi = ps_needtoplay.back();
        ps_needtoplay.pop_back();
        psi->Play();
    }
    // Destroy inactive particle systems
    while (!ps_destroy.empty())
    {
        auto& psi = *ps_destroy.begin();
        R_ASSERT(psi);
        if (psi->Locked())
        {
            Log("--locked");
            break;
        }
        psi->PSI_internal_delete();
    }
}

void IGame_Persistent::destroy_particles(const bool& all_particles)
{
    ps_needtoplay.clear();

    while (!ps_destroy.empty())
    {
        auto& psi = *ps_destroy.begin();
        R_ASSERT(psi);
        VERIFY(!psi->Locked());
        psi->PSI_internal_delete();
    }

    // delete active particles
    if (all_particles)
    {
        while (!ps_active.empty())
            (*ps_active.begin())->PSI_internal_delete();
    }
    else
    {
        size_t processed = 0;
        auto iter = ps_active.rbegin();
        while (iter != ps_active.rend())
        {
            const auto size = ps_active.size();

            auto& object = *(iter++);

            if (object->destroy_on_game_load())
                object->PSI_internal_delete();

            if (size != ps_active.size())
            {
                iter = ps_active.rbegin();
                std::advance(iter, processed);
            }
            else
            {
                processed++;
            }
        }
    }
}

void IGame_Persistent::models_savePrefetch() { Render->models_savePrefetch(); }


void IGame_Persistent::GrassBendersUpdate(const u16 id, size_t& data_idx, u32& data_frame, const Fvector& position)
{
    if (ps_ssfx_grass_interactive.y < 1.f) // Interactive grass disabled
        return;

    if (Device.dwFrame < data_frame)
    {
        // Just update position if not NULL
        if (data_idx)
        {
            // Explosions can take the mem spot, unassign and try to get a spot later.
            if (grass_shader_data.id[data_idx] != id)
            {
                data_idx = 0;
                data_frame = Device.dwFrame + Random.randI(10, 35);
            }
            else
            {
                // Just Update... ( FadeIn if str < 1.0f )
                if (grass_shader_data.dir[data_idx].w < 1.0f)
                    grass_shader_data.dir[data_idx].w += 0.5f * Device.fTimeDelta;
                else
                    grass_shader_data.dir[data_idx].w = 1.0f;

                const float saved_radius = grass_shader_data.pos[data_idx].w;
                grass_shader_data.pos[data_idx].set(position.x, position.y, position.z, saved_radius);
            }
        }

        return;
    }

    // Wait some random frames to split the checks
    data_frame = Device.dwFrame + Random.randI(10, 35);
    // Check Distance
    if (position.distance_to_xz_sqr(Device.vCameraPosition) > ps_ssfx_grass_interactive.z)
    {
        GrassBendersRemoveByIndex(data_idx);
        return;
    }

    const CFrustum& view_frust = ::Render->ViewBase;
    u32 mask = 0xff;
    // In view frustum?
    if (!view_frust.testSphere(position, 1, mask))
    {
        GrassBendersRemoveByIndex(data_idx);
        return;
    }

    // Empty slot, let's use this
    if (data_idx == 0)
    {
        const size_t idx = grass_shader_data.index + 1;
        // Add to grass blenders array
        if (grass_shader_data.id[idx] == 0)
        {
            data_idx = idx;
            GrassBendersSet(idx, id, position, {0.f, -99.f, 0.f}, 0.f, 0.f, 0.0f, 0.f, true);
            grass_shader_data.pos[idx].w = -1.0f;
        }
        // Back to 0 when the array limit is reached
        grass_shader_data.index = idx < static_cast<size_t>(ps_ssfx_grass_interactive.y) ? idx : 0;
    }
    else
    {
        // Already inview, let's add more time to re-check
        data_frame += 60;
        const float saved_radius = grass_shader_data.pos[data_idx].w;
        grass_shader_data.pos[data_idx].set(position.x, position.y, position.z, saved_radius);
    }
}

void IGame_Persistent::GrassBendersAddExplosion(const u16 id, const Fvector& position, const Fvector3& dir, const float fade, const float speed, const float intensity, const float radius)
{
    if (ps_ssfx_grass_interactive.y < 1.f)
        return;

    for (size_t idx = 1; idx < std::size(grass_shader_data.radius); ++idx)
    {
        // Add explosion to any spot not already taken by an explosion.
        if (grass_shader_data.radius[idx] == 0.f)
        {
            // Add 99 to avoid conflicts between explosions and basic benders.
            GrassBendersSet(idx, id + 99, position, dir, fade, speed, intensity, radius, true);
            grass_shader_data.str_target[idx] = intensity;
            break;
        }
    }
}

void IGame_Persistent::GrassBendersAddShot(const u16 id, const Fvector& position, const Fvector3& dir,const  float fade, const float speed, const float intensity, const float radius)
{
    // Is disabled?
    if (ps_ssfx_grass_interactive.y < 1.f || intensity <= 0.0f)
        return;

    // Check distance
    if (position.distance_to_xz_sqr(Device.vCameraPosition) > ps_ssfx_grass_interactive.z)
        return;

    size_t AddAt = size_t(-1);
    // Look for a spot
    for (size_t idx = 1; idx < std::size(grass_shader_data.id); ++idx)
    {
        // Already exist, just update and increase intensity
        if (grass_shader_data.id[idx] == id)
        {
            const float currentSTR = grass_shader_data.dir[idx].w;
            GrassBendersSet(idx, id, position, dir, fade, speed, currentSTR, radius, false);
            grass_shader_data.str_target[idx] += intensity;
            AddAt = size_t(-1);
            break;
        }
        else
        {
            // Check all index and keep usable index to use later if needed...
            if (AddAt == size_t(-1) && grass_shader_data.radius[idx] == 0.f)
                AddAt = idx;
        }
    }

    // We got an available index... Add bender at AddAt
    if (AddAt != size_t(-1))
    {
        GrassBendersSet(AddAt, id, position, dir, fade, speed, 0.001f, radius, true);
        grass_shader_data.str_target[AddAt] = intensity;
    }
}

void IGame_Persistent::GrassBendersUpdateExplosions()
{
    for (size_t idx = 1; idx < std::size(grass_shader_data.radius); ++idx)
    {
        if (grass_shader_data.radius[idx] != 0.f)
        {
            // Radius
            grass_shader_data.time[idx] += Device.fTimeDelta * grass_shader_data.speed[idx];
            grass_shader_data.pos[idx].w = grass_shader_data.radius[idx] * std::min(1.0f, grass_shader_data.time[idx]);
            grass_shader_data.str_target[idx] = std::min(1.0f, grass_shader_data.str_target[idx]);
            // Easing
            float diff = abs(grass_shader_data.dir[idx].w - grass_shader_data.str_target[idx]);
            diff = std::max(0.1f, diff);
            // Intensity
            if (grass_shader_data.str_target[idx] <= grass_shader_data.dir[idx].w)
            {
                grass_shader_data.dir[idx].w -= Device.fTimeDelta * grass_shader_data.fade[idx] * diff;
            }
            else
            {
                grass_shader_data.dir[idx].w += Device.fTimeDelta * grass_shader_data.speed[idx] * diff;
                if (grass_shader_data.dir[idx].w >= grass_shader_data.str_target[idx])
                    grass_shader_data.str_target[idx] = 0;
            }
            // Remove Bender
            if (grass_shader_data.dir[idx].w < 0.0f)
                GrassBendersReset(idx);
        }
    }
}

void IGame_Persistent::GrassBendersRemoveByIndex(size_t& idx)
{
    if (idx != 0)
    {
        GrassBendersReset(idx);
        idx = 0;
    }
}

void IGame_Persistent::GrassBendersRemoveById(const u16 id)
{
    // Search by Object ID ( Used when removing benders CPHMovementControl::DestroyCharacter() )
    for (size_t idx = 1; idx < std::size(grass_shader_data.id); ++idx)
        if (grass_shader_data.id[idx] == id)
            GrassBendersReset(idx);
}

void IGame_Persistent::GrassBendersReset(const size_t idx) { GrassBendersSet(idx, 0, {}, {0.f, -99.f, 0.f}, 0.f, 0.f, 1.f, 0.f, true); }

void IGame_Persistent::GrassBendersSet(const size_t idx, const u16 id, const Fvector& position, const Fvector3& dir, const float fade, const float speed, const float intensity, const float radius, const bool resetTime)
{
    // Set values
    const float saved_radius = grass_shader_data.pos[idx].w;
    grass_shader_data.pos[idx].set(position.x, position.y, position.z, saved_radius);
    grass_shader_data.id[idx] = id;
    grass_shader_data.radius[idx] = radius;
    grass_shader_data.fade[idx] = fade;
    grass_shader_data.speed[idx] = speed;
    grass_shader_data.dir[idx].set(dir.x, dir.y, dir.z, intensity);
    if (resetTime)
    {
        grass_shader_data.pos[idx].w = 0.01f;
        grass_shader_data.time[idx] = 0.f;
    }
}
