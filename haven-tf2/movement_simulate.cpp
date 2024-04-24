#include "movement_simulate.h"
#include "player_manager.h"
#include "movement.h"
#include "math.h"
#include <cmath>

#define COORD_FRACTIONAL_BITS 5
#define COORD_DENOMINATOR (1 << (COORD_FRACTIONAL_BITS))
#define COORD_RESOLUTION (1.0f / (COORD_DENOMINATOR))

#define DIST_EPSILON (0.03125f)

vector c_movement_simulate::get_player_mins()
{
    return mv.m_player->mins();
}
vector c_movement_simulate::get_player_maxs()
{
    return mv.m_player->maxs();
}
void c_movement_simulate::trace_player_bbox(const vector& start, const vector& end, unsigned int fMask,
                                            int collisionGroup, trace_t& pm)
{
    ray_t ray;
    ray.initialize(start, end, get_player_mins(), get_player_maxs());
    CTraceFilterIgnorePlayers traceFilter(mv.m_player, collisionGroup);
    g_interfaces.m_engine_trace->trace_ray(ray, fMask, &traceFilter, &pm);
}
void c_movement_simulate::try_touch_ground(c_base_entity* player, const vector& start, const vector& end,
                                           const vector& mins, const vector& maxs, unsigned int fMask,
                                           int collisionGroup, trace_t& pm)
{
    ray_t ray;
    ray.initialize(start, end, mins, maxs);
    CTraceFilterIgnorePlayers traceFilter(player, collisionGroup);
    g_interfaces.m_engine_trace->trace_ray(ray, fMask, &traceFilter, &pm);
}

void c_movement_simulate::try_touch_ground_in_quadrants(c_base_entity* player, const vector& start, const vector& end,
                                                        const vector& minsSrc, const vector& maxsSrc,
                                                        unsigned int fMask, int collisionGroup, trace_t& pm)
{
    const float fraction = pm.m_fraction;
    const vector endpos = pm.m_end;

    // Check the -x, -y quadrant
    vector maxs;
    vector mins = minsSrc;
    maxs.init(fminf(0, maxsSrc.m_x), fminf(0, maxsSrc.m_y), maxsSrc.m_z);
    try_touch_ground(player, start, end, mins, maxs, fMask, collisionGroup, pm);
    if (pm.m_entity && pm.m_plane.normal[2] >= 0.7)
    {
        pm.m_fraction = fraction;
        pm.m_end = endpos;
        return;
    }

    // Check the +x, +y quadrant
    mins.init(fmaxf(0, minsSrc.m_x), fmaxf(0, minsSrc.m_y), minsSrc.m_z);
    maxs = maxsSrc;
    try_touch_ground(player, start, end, mins, maxs, fMask, collisionGroup, pm);
    if (pm.m_entity && pm.m_plane.normal[2] >= 0.7)
    {
        pm.m_fraction = fraction;
        pm.m_end = endpos;
        return;
    }

    // Check the -x, +y quadrant
    mins.init(minsSrc.m_x, fmaxf(0, minsSrc.m_y), minsSrc.m_z);
    maxs.init(fminf(0, maxsSrc.m_x), maxsSrc.m_y, maxsSrc.m_z);
    try_touch_ground(player, start, end, mins, maxs, fMask, collisionGroup, pm);
    if (pm.m_entity && pm.m_plane.normal[2] >= 0.7)
    {
        pm.m_fraction = fraction;
        pm.m_end = endpos;
        return;
    }

    // Check the +x, -y quadrant
    mins.init(fmaxf(0, minsSrc.m_x), minsSrc.m_y, minsSrc.m_z);
    maxs.init(maxsSrc.m_x, fminf(0, maxsSrc.m_y), maxsSrc.m_z);
    try_touch_ground(player, start, end, mins, maxs, fMask, collisionGroup, pm);
    if (pm.m_entity && pm.m_plane.normal[2] >= 0.7)
    {
        pm.m_fraction = fraction;
        pm.m_end = endpos;
        return;
    }

    pm.m_fraction = fraction;
    pm.m_end = endpos;
}

#define CONTENTS_REDTEAM CONTENTS_TEAM1
#define CONTENTS_BLUETEAM CONTENTS_TEAM2

unsigned int c_movement_simulate::player_solid_mask()
{
    unsigned int uMask = 0;

    if (mv.m_player)
    {
        // Ghost players dont collide with anything but the world
        if (mv.m_player && mv.m_player->in_cond(TF_COND_HALLOWEEN_GHOST_MODE))
        {
            return MASK_PLAYERSOLID_BRUSHONLY;
        }

        switch (mv.m_player->m_i_team_num())
        {
            case 2:
                uMask = CONTENTS_BLUETEAM;
                break;

            case 3:
                uMask = CONTENTS_REDTEAM;
                break;
        }
    }

    return (uMask | MASK_PLAYERSOLID);
}

int c_movement_simulate::clip_velocity(vector& in, vector& normal, vector& out, float overbounce)
{

    const float angle = normal[2];

    int blocked = 0x00;  // Assume unblocked.
    if (angle > 0)       // If the plane that is blocking us has a positive z component, then assume it's a floor.
        blocked |= 0x01; //
    if (angle == 0.f)    // If the plane has no Z, it is vertical (wall/step)
        blocked |= 0x02; //

    // Determine how far along plane to slide based on incoming direction.
    const float backoff = in.dot(normal) * overbounce;

    for (int i = 0; i < 3; i++)
    {
        const float change = normal[i] * backoff;
        out[i] = in[i] - change;
    }

    // iterate once to make sure we aren't still moving through the plane
    const float adjust = out.dot(normal);
    if (adjust < 0.0f)
    {
        out -= (normal * adjust);
        //		Msg( "Adjustment = %lf\n", adjust );
    }

    // Return blocking flags.
    return blocked;
}

int c_movement_simulate::try_player_move()
{
    vector dir;
    float d;

    vector planes[MAX_CLIP_PLANES];
    vector primal_velocity, original_velocity;
    vector new_velocity;
    int i, j;
    trace_t pm;
    vector end;
    float time_left, allFraction;
    int blocked;

    blocked = 0;       // Assume not blocked
    int numplanes = 0; //  and not sliding along any planes

    original_velocity = mv.m_velocity; // Store original velocity
    primal_velocity = mv.m_velocity;

    allFraction = 0;
    time_left = g_interfaces.m_global_vars->m_interval_per_tick; // Total time for this movement operation.

    new_velocity.init();

    for (int bumpcount = 0; bumpcount < 4; bumpcount++)
    {
        if (mv.m_velocity.length() == 0.f)
            break;

        // Assume we can move all the way from the current origin to the
        //  end point.
        end = mv.m_position.vector_ma(time_left, mv.m_velocity);

        // See if we can make it from origin to end point.
        trace_player_bbox(mv.m_position, end, player_solid_mask(), COLLISION_GROUP_PLAYER_MOVEMENT, pm);

        allFraction += pm.m_fraction;

        // If we started in a solid object, or we were in solid space
        //  the whole way, zero out our velocity and return that we
        //  are blocked by floor and wall.
        if (pm.m_allsolid)
        {
            // entity is trapped in another solid
            mv.m_velocity.init();
            return 4;
        }

        // If we moved some portion of the total distance, then
        //  copy the end position into the pmove.origin and
        //  zero the plane counter.
        if (pm.m_fraction > 0)
        {
            if (pm.m_fraction == 1.f)
            {
                // There's a precision issue with terrain tracing that can cause a swept box to successfully trace
                // when the end position is stuck in the triangle.  Re-run the test with an uswept box to catch that
                // case until the bug is fixed.
                // If we detect getting stuck, don't allow the movement
                trace_t stuck;
                trace_player_bbox(pm.m_end, pm.m_end, player_solid_mask(), COLLISION_GROUP_PLAYER_MOVEMENT, stuck);
                if (stuck.m_start_solid || stuck.m_fraction != 1.0f)
                {
                    // Msg( "Player will become stuck!!!\n" );
                    mv.m_velocity.init();
                    break;
                }
            }
            // actually covered some distance
            mv.m_position = (pm.m_end);
            mv.m_velocity = original_velocity;
            numplanes = 0;
        }

        // If we covered the entire distance, we are done
        //  and can return.
        if (pm.m_fraction == 1.f)
        {
            break; // moved the entire distance
        }

        // If the plane we hit has a high z component in the normal, then
        //  it's probably a floor
        if (pm.m_plane.normal[2] > 0.7f)
        {
            blocked |= 1; // floor
        }
        // If the plane has a zero z component in the normal, then it's a
        //  step or wall
        if (0.f != pm.m_plane.normal[2])
        {
            blocked |= 2; // step / wall
        }

        // Reduce amount of m_flFrameTime left by total time left * fraction
        //  that we covered.
        time_left -= time_left * pm.m_fraction;

        // Did we run out of planes to clip against?
        if (numplanes >= MAX_CLIP_PLANES)
        {
            // this shouldn't really happen
            //  Stop our movement if so.
            mv.m_velocity.init();
            // Con_DPrintf("Too many planes 4\n");

            break;
        }

        // Set up next clipping plane
        planes[numplanes] = pm.m_plane.normal;
        numplanes++;

        // modify original_velocity so it parallels all of the clip planes
        //

        // reflect player velocity
        // Only give this a try for first impact plane because you can get yourself stuck in an acute corner by jumping
        // in place
        //  and pressing forward and nobody was really using this bounce/reflection feature anyway...
        if (numplanes == 1 && mv.on_ground == false)
        {
            for (i = 0; i < numplanes; i++)
            {
                if (planes[i][2] > 0.7f)
                {
                    // floor or slope
                    clip_velocity(original_velocity, planes[i], new_velocity, 1);
                    original_velocity = new_velocity;
                }
                else
                {
                    static auto sv_bounce = g_interfaces.m_cvar->find_var("sv_bounce");
                    clip_velocity(original_velocity, planes[i], new_velocity,
                                  1.f + sv_bounce->m_value.m_float_value * (1 - mv.m_surface_friction));
                }
            }

            mv.m_velocity = new_velocity;
            original_velocity = new_velocity;
        }
        else
        {
            for (i = 0; i < numplanes; i++)
            {
                clip_velocity(original_velocity, planes[i], mv.m_velocity, 1);

                for (j = 0; j < numplanes; j++)
                    if (j != i)
                    {
                        // Are we now moving against this plane?
                        if (mv.m_velocity.dot(planes[j]) < 0.f)
                            break; // not ok
                    }
                if (j == numplanes) // Didn't have to clip, so we're ok
                    break;
            }

            // Did we go all the way through plane set
            if (i != numplanes)
            { // go along this plane
              // pmove.velocity is set in clipping call, no need to set again.
            }
            else
            { // go along the crease
                if (numplanes != 2)
                {
                    mv.m_velocity.init();
                    break;
                }
                planes[0].cross_product(planes[1], dir);
                dir.normalize_in_place();
                d = dir.dot(mv.m_velocity);
                mv.m_velocity = dir * d;
            }

            //
            // if original velocity is against the original velocity, stop dead
            // to avoid tiny occilations in sloping corners
            //
            d = mv.m_velocity.dot(primal_velocity);
            if (d <= 0.f)
            {
                // Con_DPrintf("Back\n");
                mv.m_velocity.init();
                break;
            }
        }
    }

    if (allFraction == 0.f)
    {
        mv.m_velocity.init();
    }

    return blocked;
}
void c_movement_simulate::friction(void)
{

    // apply ground friction
    if (mv.on_ground) // On an entity that is the ground
    {
        // Calculate speed
        const float speed = mv.m_velocity.length();

        // If too slow, return
        if (speed < 0.1f)
        {
            return;
        }
        float drop = 0.f;
        const float friction = sv_friction->m_value.m_float_value * mv.m_surface_friction;

        // Bleed off some speed, but if we have less than the bleed
        //  threshold, bleed the threshold amount.

        const float control =
            (speed < sv_stopspeed->m_value.m_float_value) ? sv_stopspeed->m_value.m_float_value : speed;

        // Add the amount to the drop amount.
        drop += control * friction * g_interfaces.m_global_vars->m_interval_per_tick;

        // scale the velocity
        float newspeed = speed - drop;
        if (newspeed < 0.f)
            newspeed = 0;

        if (newspeed != speed)
        {
            // Determine proportion of old speed we are using.
            newspeed /= speed;
            // Adjust velocity according to proportion.
            mv.m_velocity *= newspeed;
        }
    }
}

void c_movement_simulate::categorize_position(void)
{
    trace_t pm;

    // Reset this each time we-recategorize, otherwise we have bogus friction when we jump into water and plunge
    // downward really quickly
    mv.m_surface_friction = 1.f;

    const float zvel = mv.m_velocity[2];

    // Was on ground, but now suddenly am not.  If we hit a steep plane, we are not on ground
    if (zvel > 250.0f)
    {
        SetGroundEntity(nullptr);
        return;
    }
    vector vecStartPos = mv.m_position;
    vector vecEndPos(mv.m_position.m_x, mv.m_position.m_y, (mv.m_position.m_z - 2.0f));
    bool bMoveToEndPos = false;
    if (mv.on_ground)
    {
        vecEndPos.m_z -= 16.f + DIST_EPSILON;
        bMoveToEndPos = true;
    }
    try_touch_ground(mv.m_player, vecStartPos, vecEndPos, get_player_mins(), get_player_maxs(), MASK_PLAYERSOLID,
                     COLLISION_GROUP_PLAYER_MOVEMENT, pm);
    if (pm.m_plane.normal[2] < 0.7f)
    {
        // Test four sub-boxes, to see if any of them would have found shallower slope we could actually stand on
        try_touch_ground_in_quadrants(mv.m_player, vecStartPos, vecEndPos, get_player_mins(), get_player_maxs(),
                                      MASK_PLAYERSOLID, COLLISION_GROUP_PLAYER_MOVEMENT, pm);

        if (pm.m_entity == nullptr || pm.m_plane.normal[2] < 0.7f)
        {
            SetGroundEntity(nullptr);
            // probably want to add a check for a +z velocity too!
            if ((mv.m_velocity.m_z > 0.0f))
            {
                mv.m_surface_friction = 0.25f;
            }
        }
        else
        {
            SetGroundEntity(&pm);
        }
    }
    else
    {
        if (bMoveToEndPos && !pm.m_start_solid && // not sure we need this check as fraction would == 0.0f?
            pm.m_fraction > 0.0f &&               // must go somewhere
            pm.m_fraction < 1.0f)                 // must hit something
        {
            float flDelta = fabs(mv.m_position.m_z - pm.m_end.m_z);
            // HACK HACK:  The real problem is that trace returning that strange value
            //  we can't network over based on bit precision of networking origins
            if (flDelta > 0.5f * COORD_RESOLUTION)
            {
                vector org = mv.m_position;
                org.m_z = pm.m_end.m_z;
                mv.m_position = org;
            }
        }
        SetGroundEntity(&pm);
    }
}
void c_movement_simulate::check_velocity(void)
{

    //
    // bound velocity
    //

    for (int i = 0; i < 3; i++)
    {
        // See if it's bogus.
        if (isnan(mv.m_velocity[i]))
        {
            mv.m_velocity[i] = 0;
        }

        if (isnan(mv.m_position[i]))
        {
            mv.m_position[i] = 0;
        }

        // Bound it.
        if (mv.m_velocity[i] > sv_maxvelocity->m_value.m_float_value)
            mv.m_velocity[i] = sv_maxvelocity->m_value.m_float_value;

        else if (mv.m_velocity[i] < -sv_maxvelocity->m_value.m_float_value)
            mv.m_velocity[i] = -sv_maxvelocity->m_value.m_float_value;
    }
}
void c_movement_simulate::start_gravity(void)
{
    float ent_gravity = 1.f;

    if (mv.m_player->in_cond(TF_COND_PARACHUTE_DEPLOYED))
        ent_gravity = 0.5f;
    // Add gravity so they'll be in the correct position during movement
    // yes, this 0.5 looks wrong, but it's not.
    mv.m_velocity[2] -=
        (ent_gravity * sv_gravity->m_value.m_float_value * 0.5f * g_interfaces.m_global_vars->m_interval_per_tick);
    mv.m_velocity[2] += mv.m_base_velocity[2] * g_interfaces.m_global_vars->m_interval_per_tick;

    mv.m_base_velocity[2] = 0;

    check_velocity();
}
void c_movement_simulate::finish_gravity(void)
{
    float ent_gravity = 1.f;

    // if ( player->m_flWaterJumpTime )
    //	return;
    //
    // if ( player->GetGravity( ) )
    //	ent_gravity = player->GetGravity( );
    // else
    if (mv.m_player->in_cond(TF_COND_PARACHUTE_DEPLOYED))
        ent_gravity = 0.5f;

    // Get the correct velocity for the end of the dt
    mv.m_velocity[2] -=
        (ent_gravity * sv_gravity->m_value.m_float_value * 0.5f * g_interfaces.m_global_vars->m_interval_per_tick);

    check_velocity();
}

void c_movement_simulate::stay_on_ground(void)
{
    trace_t trace;
    vector start = mv.m_position;
    vector end = mv.m_position;
    start.m_z += 2;
    end.m_z -= 16.f + DIST_EPSILON; // player->GetStepSize( );

    // See how far up we can go without getting stuck

    trace_player_bbox(mv.m_position, start, player_solid_mask(), COLLISION_GROUP_PLAYER_MOVEMENT, trace);
    start = trace.m_end;

    // using trace.startsolid is unreliable here, it doesn't get set when
    // tracing bounding box vs. terrain

    // Now trace down from a known safe position
    trace_player_bbox(start, end, player_solid_mask(), COLLISION_GROUP_PLAYER_MOVEMENT, trace);
    if (trace.m_fraction > 0.0f &&       // must go somewhere
        trace.m_fraction < 1.0f &&       // must hit something
        !trace.m_start_solid &&          // can't be embedded in a solid
        trace.m_plane.normal[2] >= 0.7f) // can't hit a steep slope that we can't stand on anyway
    {
        const float flDelta = fabs(mv.m_position.m_z - trace.m_end.m_z);

        // This is incredibly hacky. The real problem is that trace returning that strange value we can't network over.
        if (flDelta > 0.5f * COORD_RESOLUTION)
        {
            mv.m_position = trace.m_end;
        }
    }
}

void c_movement_simulate::air_acceletate(vector& wishdir, const float wishspeed, const float accel)
{
    float wishspd = wishspeed;

    // Cap speed
    if (wishspd > 30.f)
        wishspd = 30.f;

    // Determine veer amount
    const float currentspeed = mv.m_velocity.dot(wishdir);

    // See how much to add
    const float addspeed = wishspd - currentspeed;

    // If not adding any, done.
    if (addspeed <= 0)
        return;

    // Determine acceleration speed after acceleration
    float accelspeed = accel * wishspeed * g_interfaces.m_global_vars->m_interval_per_tick;

    // Cap it
    if (accelspeed > addspeed)
        accelspeed = addspeed;

    // Adjust pmove vel.
    for (int i = 0; i < 2; i++)
    {
        mv.m_velocity[i] += accelspeed * wishdir[i];
    }
}

void c_movement_simulate::air_move()
{
    vector forward, right, up;
    vector(0, mv.m_eye_dir, 0).angle_vectors(&forward, &right, &up); // Determine movement angles
    float fmove = 0;
    float smove = 0;
    float len_2d = mv.m_velocity.length_2d();
    if (len_2d > 5.f && fabsf(mv.m_dir) > 0.f)
    {
        // Copy movement amounts
        fmove = mv.m_air_dir.m_x;
        smove = mv.m_air_dir.m_y;
    }

    // Zero out z components of movement vectors
    forward[2] = 0;
    right[2] = 0;
    forward.normalize_in_place(); // Normalize remainder of vectors
    right.normalize_in_place();   //

    vector wishvel, wishdir;
    for (int i = 0; i < 2; i++) // Determine x and y parts of velocity
        wishvel[i] = forward[i] * fmove + right[i] * smove;
    wishvel[2] = 0; // Zero out z part of velocity

    wishdir = wishvel; // Determine maginitude of speed of move
    mv.m_walk_direction = wishdir;
    float wishspeed = wishdir.normalize_in_place();

    // vector wishdir = wishvel;   // Determine maginitude of speed of move
    // float wishspeed = wishdir.normalize_in_place( );

    air_acceletate(wishdir, wishspeed, sv_airaccelerate->m_value.m_float_value);
    // mv.m_velocity += mv.m_base_velocity;
    //  vector look = vector( ).look( mv.m_velocity );
    //  look.m_y += mv.m_dir;
    //  mv.m_velocity = look.angle_vector() * mv.m_velocity.length();
    try_player_move();
}

void c_movement_simulate::accelerate(vector& wishdir, const float wishspeed, const float accel)
{

    // See if we are changing direction a bit
    const float currentspeed = mv.m_velocity.dot(wishdir);
    ;

    // Reduce wishspeed by the amount of veer.
    const float addspeed = wishspeed - currentspeed;

    // If not going to add any speed, done.
    if (addspeed <= 0)
        return;

    // Determine amount of accleration.
    float accelspeed = accel * g_interfaces.m_global_vars->m_interval_per_tick * wishspeed * mv.m_surface_friction;

    // Cap at addspeed
    if (accelspeed < addspeed)
        accelspeed = addspeed;

    // Adjust velocity.
    for (int i = 0; i < 3; i++)
    {
        mv.m_velocity[i] += accelspeed * wishdir[i];
    }
}

int vector_compare(const vector& v1,
                   const vector& v2) // umm floating point imprecision anyone??
{
    for (int i = 0; i < 3; i++)
        if (v1[i] != v2[i])
            return 0;

    return 1;
}

void c_movement_simulate::step_move(vector target, trace_t& trace)
{
    bool low_road = false, up_road = true;

    vector end_pos = target;
    vector pos = mv.m_position, vel = mv.m_velocity;

    // Move up a stair height.
    end_pos.m_z += 18.f + DIST_EPSILON; // player->m_Local.m_flStepSize + DIST_EPSILON;

    trace_player_bbox(mv.m_position, end_pos, player_solid_mask(), COLLISION_GROUP_PLAYER_MOVEMENT, trace);
    if (!trace.m_start_solid && !trace.m_allsolid)
    {
        mv.m_position = trace.m_end;
    }

    // Slide move up.
    try_player_move();

    // Move down a stair (attempt to).
    end_pos = mv.m_position;
    end_pos.m_z -= 18.f + DIST_EPSILON; // player->m_Local.m_flStepSize + DIST_EPSILON;

    trace_player_bbox(mv.m_position, end_pos, player_solid_mask(), COLLISION_GROUP_PLAYER_MOVEMENT, trace);
    if (!trace.m_start_solid && !trace.m_allsolid)
    {
        mv.m_position = trace.m_end;
    }

    if ((trace.m_fraction != 1.f && trace.m_plane.normal.m_z < 0.7f) || vector_compare(mv.m_position, pos))
    {
        low_road = true;
        up_road = false;
    }

    if (low_road)
    {
        vector up_pos, up_vel;
        if (up_road)
        {
            up_pos = mv.m_position;
            up_vel = mv.m_velocity;
        }

        mv.m_position = pos;
        mv.m_velocity = vel;
        target = end_pos;
        try_player_move();

        vector down_pos, down_vel;
        down_pos = mv.m_position;
        down_vel = mv.m_velocity;

        if (up_road)
        {
            float up_dist =
                (up_pos.m_x - pos.m_x) * (up_pos.m_x - pos.m_x) + (up_pos.m_y - pos.m_y) * (up_pos.m_y - pos.m_y);
            float down_dist = (down_pos.m_x - pos.m_x) * (down_pos.m_x - pos.m_x) +
                              (down_pos.m_y - pos.m_y) * (down_pos.m_y - pos.m_y);

            if (up_dist >= down_dist)
            {
                mv.m_position = up_pos;
                mv.m_velocity = up_vel;

                mv.m_velocity.m_z = down_vel.m_z;
            }
        }
    }
}

float c_movement_simulate::calc_wish_speed_threshold()
{
    return 100.0f * sv_friction->m_value.m_float_value / (sv_accelerate->m_value.m_float_value);
}

#define TIME_TO_TICKS(dt) ((int)(0.5f + (float)(dt) / g_interfaces.m_global_vars->m_interval_per_tick))

void c_movement_simulate::walk_move()
{

    vector wishvel;
    float spd;

    vector dest;
    trace_t pm;
    vector forward, right, up;
    vector(0, mv.m_eye_dir, 0).angle_vectors(&forward, &right, &up); // Determine movement angles

    
    if (mv.m_walk_direction.length_2d() > 1.f)
    {
        if (mv.ground_circle_prediction)
        {
            float wish_dir_angle = atan2(mv.m_walk_direction.m_y, mv.m_walk_direction.m_x);

            float rot = (g_interfaces.m_global_vars->m_interval_per_tick * 360.f) / mv.ground_rotation_timing;
            if (!mv.ground_rotating_right)
                rot *= -1.f;

            wish_dir_angle += deg_to_rad(rot);

            mv.m_walk_direction.m_x = cos(wish_dir_angle) * 450.f;
            mv.m_walk_direction.m_y = sin(wish_dir_angle) * 450.f;
            mv.m_walk_direction.m_z = 0.f;
        }
        else if (mv.ground_rotation_timing > 0.f)
        {
            if (mv.m_time > mv.ground_rotation_timing)
            {
                float wish_dir_angle = atan2(mv.m_walk_direction.m_y, mv.m_walk_direction.m_x);

                float rot = mv.ground_rotating_right ? 45.f : -45.f;
                wish_dir_angle += deg_to_rad(rot);

                mv.m_walk_direction.m_x = cos(wish_dir_angle) * 450.f;
                mv.m_walk_direction.m_y = sin(wish_dir_angle) * 450.f;
                mv.m_walk_direction.m_z = 0.f;
                mv.ground_rotation_timing = -1.f;
            }
        }
    }

    const bool old_ground = mv.on_ground;

    // vector look = vector( ).look( mv.m_walk_direction );
    // look.m_y += mv.m_ground_dir;
    // mv.m_walk_direction = look.angle_vector( ) * mv.m_walk_direction.length( );
    // mv.m_accumulated_ground += mv.m_ground_dir;

    /*
    if (mv.m_ground_dir > 0)
    {
        if (mv.m_accumulated_ground > 45.f)
        {
            temp.m_y += 45.f;
            mv.m_accumulated_ground -= 45.f;
        }
    }
    else
    {
        if (mv.m_accumulated_ground < -45.f)
        {
            temp.m_y -= 45.f;
            mv.m_accumulated_ground += 45.f;
        }
    }
    */
    wishvel = mv.m_walk_direction;
    wishvel.m_z = 0.f;

    vector wishdir = wishvel; // Determine maginitude of speed of move
    float wishspeed = wishdir.normalize_in_place();

    wishspeed = std::clamp<float>(wishspeed, 0.0f, mv.m_max_speed);

    // Accelerate in the x,y plane.
    mv.m_velocity[2] = 0;

    float flAccelerate = sv_accelerate->m_value.m_float_value;
    // if our wish speed is too low (attributes), we must increase acceleration or we'll never overcome friction
    // Reverse the basic friction calculation to find our required acceleration
    if (wishspeed > 0 && wishspeed < calc_wish_speed_threshold())
    {
        // accelspeed = accel * gpGlobals->frametime * wishspeed * player->m_surfaceFriction;
        // accelspeed > drop;
        // drop = accel * frametime * wish * plFriction
        // accel > drop / (wish * gametime * plFriction)
        //		drop = control * (plFriction * sv_friction) * gameTime;
        // accel > control * sv_friction / wish
        const float fl_speed = mv.m_velocity.length();
        const float fl_control =
            (fl_speed < sv_stopspeed->m_value.m_float_value) ? sv_stopspeed->m_value.m_float_value : fl_speed;
        flAccelerate = (fl_control * sv_friction->m_value.m_float_value) / wishspeed + 1;
    }

    // Set pmove velocity
    accelerate(wishdir, wishspeed, flAccelerate);

    mv.m_velocity[2] = 0;

    float flNewSpeed = mv.m_velocity.length();
    if ((flNewSpeed != 0.0f) && (flNewSpeed > mv.m_max_speed))
    {
        float flScale = (mv.m_max_speed / flNewSpeed);
        mv.m_velocity.m_x *= flScale;
        mv.m_velocity.m_y *= flScale;
    }

    if (tf_clamp_back_speed->m_value.m_float_value < 1.0f &&
        mv.m_velocity.length() > tf_clamp_back_speed_min->m_value.m_float_value)
    {
        float flDot = forward.dot(mv.m_velocity);

        // are we moving backwards at all?
        if (flDot < 0)
        {
            vector vecBackMove = forward * flDot;
            vector vecRightMove = right * right.dot(mv.m_velocity);

            // clamp the back move vector if it is faster than max
            float flBackSpeed = vecBackMove.length();
            float flMaxBackSpeed = (mv.m_max_speed * tf_clamp_back_speed->m_value.m_float_value);

            if (flBackSpeed > flMaxBackSpeed)
            {
                vecBackMove *= flMaxBackSpeed / flBackSpeed;
            }

            // reassemble velocity
            mv.m_velocity = vecBackMove + vecRightMove;

            // Re-run this to prevent crazy values (clients can induce this via usercmd viewangles hacking)
            flNewSpeed = mv.m_velocity.length();
            if (flNewSpeed > mv.m_max_speed)
            {
                float flScale = (mv.m_max_speed / flNewSpeed);
                mv.m_velocity.m_x *= flScale;
                mv.m_velocity.m_y *= flScale;
            }
        }
    }

    mv.m_velocity += mv.m_base_velocity;

    // Add in any base velocity to the current velocity.

    // spd = mv.m_velocity.length( );
    //
    // if ( spd < 1.0f ) {
    //	mv.m_velocity.init( );
    //	return;
    // }

    // first try just moving to the destination
    dest[0] = mv.m_position[0] + mv.m_velocity[0] * g_interfaces.m_global_vars->m_interval_per_tick;
    dest[1] = mv.m_position[1] + mv.m_velocity[1] * g_interfaces.m_global_vars->m_interval_per_tick;
    dest[2] = mv.m_position[2];

    // first try moving directly to the next spot
    trace_player_bbox(mv.m_position, dest, player_solid_mask(), COLLISION_GROUP_PLAYER_MOVEMENT, pm);

    if (pm.m_fraction == 1)
    {
        mv.m_position = pm.m_end;
        stay_on_ground();
        mv.m_velocity -= mv.m_base_velocity;
        return;
    }

    if (!old_ground)
    {
        // Now pull the base velocity back out.   Base velocity is set if you are on a moving object, like a conveyor
        // (or maybe another monster?)
        mv.m_velocity -= mv.m_base_velocity;
        return;
    }

    step_move(dest, pm);
    // try_player_move( );

    stay_on_ground();
    mv.m_velocity -= mv.m_base_velocity;
}

void c_movement_simulate::full_walk_move()
{
    start_gravity();

    if (mv.on_ground)
    {
        mv.m_velocity[2] = 0.0;
        friction();
    }

    if (!mv.on_ground)
        air_move();
    else
        walk_move();

    categorize_position();

    check_velocity();

    finish_gravity();

    if (mv.on_ground)
        mv.m_velocity[2] = 0.0;
}

// void c_movement_simulate::SetGroundEntity( trace_t* pm ) {
//	c_base_entity* newGround = pm ? pm->m_entity : NULL;
//
//	c_base_entity* oldGround = mv.m_ground;
//	vector vecBaseVelocity = player->GetBaseVelocity( );
//
//	if ( !oldGround && newGround ) {
//		// Subtract ground velocity at instant we hit ground jumping
//		vecBaseVelocity -= newGround->GetAbsVelocity( );
//		vecBaseVelocity.m_z = newGround->GetAbsVelocity( ).z;
//	}
//	else if ( oldGround && !newGround ) {
//		// Add in ground velocity at instant we started jumping
//		vecBaseVelocity += oldGround->GetAbsVelocity( );
//		vecBaseVelocity.m_z = oldGround->GetAbsVelocity( ).z;
//	}
//
//	mv.m_base_vel = ( vecBaseVelocity );
//	mv.m_ground = ( newGround );
//
//	// If we are on something...
//
//	if ( newGround ) {
//		//CategorizeGroundSurface( *pm );
//
//		// Standing on an entity other than the world, so signal that we are touching something.
//		//if ( !pm->DidHitWorld( ) ) {
//		//	MoveHelper( )->AddToTouched( *pm, mv->m_vecVelocity );
//		//}
//
//		mv.m_velocity.m_z = 0.0f;
//	}
// }

void c_movement_simulate::player_move()
{
    if (mv.m_velocity.m_z > 250.0f)
    {
        SetGroundEntity(nullptr);
    }

    full_walk_move();
}

bool c_movement_simulate::charge_move()
{
    if (!mv.m_player->in_cond(TF_COND_SHIELD_CHARGE))
    {
        return false;
        // Check for Quick Fix Medic healing a charging player
        if (mv.m_player->m_class() != TF2_Medic)
            return false;

        auto* weapon = mv.m_player->get_active_weapon();
        if (!weapon)
            return false;

        if (weapon->item_index() != medicweapons::WPN_QuickFix)
            return false;

        // auto* target = ;
        // if (!pHealTarget || !pHealTarget->m_Shared.InCond(TF_COND_SHIELD_CHARGE))
        //     return false;
    }

    mv.m_max_speed = tf_max_charge_speed->m_value.m_float_value;

    // Handle demoman shield charge.
    mv.m_air_dir = mv.m_walk_direction = vector(tf_max_charge_speed->m_value.m_float_value, 0.0f, 0.0f);

    return true;
}

void c_movement_simulate::process_movment()
{
    charge_move();
}

float c_movement_simulate::max_speed(c_base_player* player)
{
    float base_speed = player->m_max_speed();
    if (player->flags() & (1 << 1))
        base_speed /= 3.f;
    return base_speed;
}

float dir_turning1(vector vel, vector last_vel)
{
    if (vel.length() < DIST_EPSILON)
        return 0;
    vel.normalize_in_place();
    last_vel.normalize_in_place();
    if (vel.operator==(last_vel))
        return 0.f;
    const vector delta = last_vel - vel;
    if (delta <= 0.1f)
    {
        return 0.f;
    }
    float dir = rad_to_deg(std::atan2(vel.m_y, vel.m_x)) - rad_to_deg(std::atan2(last_vel.m_y, last_vel.m_x));
    while (dir > 180.f)
        dir -= 360.f;
    while (dir < -180.f)
        dir += 360.f;
    return dir;
}

void c_movement_simulate::SetGroundEntity(trace_t* pm)
{
    auto* oldGround = mv.m_ground;
    auto* newGround = pm ? pm->m_entity : NULL;
    if (oldGround && newGround != oldGround)
    {
        if (oldGround->get_client_class()->m_class_id == CFuncConveyor)
        {
            vector right;
            oldGround->m_ang_rot().angle_vectors(nullptr, &right, nullptr);
            right *= ((c_func_conveyor*)oldGround)->conveyor_speed();
            mv.m_base_velocity -= right;
            mv.m_base_velocity.m_z = right.m_z;
        }
    }
    if (newGround && oldGround != newGround)
    {
        if (newGround->get_client_class()->m_class_id == CFuncConveyor)
        {
            vector right;
            newGround->m_ang_rot().angle_vectors(nullptr, &right, nullptr);
            right *= ((c_func_conveyor*)newGround)->conveyor_speed();
            mv.m_base_velocity += right;
            mv.m_base_velocity.m_z = right.m_z;
        }
    }
    mv.m_ground = newGround;
    mv.on_ground = mv.m_ground != nullptr;
    // mv.m_velocity.m_z = 0.0f;
}

vector c_movement_simulate::estimate_walking_dir(vector velocity, vector last_fric_vel, vector eye_ang, vector origin)
{
    if (!grab_convars())
    {
        return vector();
    }

    vector wishvel = velocity - last_fric_vel;
    float len = wishvel.normalize_in_place();
    if (len <= 1.f)
        return vector();
    return wishvel * 636.396103f;;
}

vector c_movement_simulate::estimate_air_dir(vector velocity, vector last_fric_vel, vector eye_ang, vector origin)
{
    vector forward, right, up;
    eye_ang.angle_vectors(&forward, &right, &up); // Determine movement angles

    vector best;
    float best_dist = FLT_MAX;
    for (auto f = 0; f < 3; f++)
    {
        for (auto s = 0; s < 3; s++)
        {
            mv.m_position = origin;
            mv.m_velocity = last_fric_vel;
            // Copy movement amounts
            float fmove = 0.f;
            float smove = 0.f;
            if (f == 1)
                fmove = 450.f;
            if (f == 2)
                fmove = -450.f;
            if (s == 1)
                smove = 450.f;
            if (s == 2)
                smove = -450.f;

            // Zero out z components of movement vectors
            forward[2] = 0;
            right[2] = 0;
            forward.normalize_in_place(); // Normalize remainder of vectors
            right.normalize_in_place();   //

            vector wishvel, wishdir;
            for (int i = 0; i < 2; i++) // Determine x and y parts of velocity
                wishvel[i] = forward[i] * fmove + right[i] * smove;
            wishvel[2] = 0; // Zero out z part of velocity

            wishdir = wishvel; // Determine maginitude of speed of move
            float wishspeed = wishdir.normalize_in_place();

            air_acceletate(wishdir, wishspeed, sv_airaccelerate->m_value.m_float_value);
            mv.m_velocity[2] = 0;
            float dst = (velocity - mv.m_velocity).length_2d();
            if (dst < best_dist)
            {
                best.init(fmove, smove, 0);
                best_dist = dst;
            }
        }
    }
    return best;
}

vector c_movement_simulate::find_unstuck(vector origin)
{
    trace_t stuck;
    trace_player_bbox(origin, origin, player_solid_mask(), COLLISION_GROUP_PLAYER_MOVEMENT, stuck);
    if (stuck.m_start_solid || stuck.m_fraction != 1.0f)
    {
        for (auto f = -1; f < 2; f++)
        {
            for (auto s = -1; s < 2; s++)
            {
                for (auto z = -1; z < 2; z++)
                {
                    vector cur = origin;
                    cur.m_z += f * DIST_EPSILON;
                    cur.m_y += s * DIST_EPSILON;
                    cur.m_x += z * DIST_EPSILON;
                    trace_player_bbox(cur, cur, player_solid_mask(), COLLISION_GROUP_PLAYER_MOVEMENT, stuck);
                    if (!stuck.m_start_solid && stuck.m_fraction == 1.0f)
                    {
                        return cur;
                    }
                }
            }
        }
    }
    return origin;
}

#define TIME_TO_TICKS(dt) ((int)(0.5f + (float)(dt) / g_interfaces.m_global_vars->m_interval_per_tick))
#define TICKS_TO_TIME(dt) ((int)((float)(dt) * g_interfaces.m_global_vars->m_interval_per_tick))

bool c_movement_simulate::grab_convars()
{
    if (!sv_gravity || !sv_airaccelerate || !sv_accelerate || !sv_stopspeed || !sv_friction || !tf_clamp_back_speed ||
        !tf_clamp_back_speed_min || !sv_maxvelocity || !tf_max_charge_speed)
    {
        sv_airaccelerate = g_interfaces.m_cvar->find_var("sv_airaccelerate");
        sv_gravity = g_interfaces.m_cvar->find_var("sv_gravity");
        sv_accelerate = g_interfaces.m_cvar->find_var("sv_accelerate");
        sv_stopspeed = g_interfaces.m_cvar->find_var("sv_stopspeed");
        sv_friction = g_interfaces.m_cvar->find_var("sv_friction");
        tf_clamp_back_speed = g_interfaces.m_cvar->find_var("tf_clamp_back_speed");
        tf_clamp_back_speed_min = g_interfaces.m_cvar->find_var("tf_clamp_back_speed_min");
        tf_max_charge_speed = g_interfaces.m_cvar->find_var("tf_max_charge_speed");
        sv_maxvelocity = g_interfaces.m_cvar->find_var("sv_maxvelocity");
    }
    return sv_gravity && sv_airaccelerate && sv_accelerate && sv_stopspeed && sv_friction && tf_clamp_back_speed &&
           tf_clamp_back_speed_min && sv_maxvelocity && tf_max_charge_speed;
}

bool c_movement_simulate::setup_mv(vector last_vel, c_base_player* player)
{
    mv.m_time = 0;

    if (!grab_convars())
        return false;

    const auto& player_info = g_player_manager.players[player->entindex() - 1];
    if (player_info.m_records.size() < 2)
        return false;

    const auto& record = player_info.m_records[0];

    mv.m_accumulated_ground = 0.f;
    mv.m_player = player;
    mv.on_ground = player_info.m_ground != nullptr;
    mv.m_ground = player_info.m_ground;
    mv.m_base_velocity = player_info.m_base_velocity;
    mv.m_eye_dir = record->eye_angle.m_y;
    mv.m_max_speed = max_speed(player);
    mv.m_walk_direction.init();
    mv.m_decay = 0;
    mv.ground_circle_prediction = false;
    mv.ground_rotating_right = 0.f;
    mv.ground_rotation_timing = 0.f;
    if (player_info.m_records.size() > 1)
    {
        do_angle_prediction(record, player_info);
        if (mv.on_ground)
            ground_input_prediction(player_info, record);
        else
            air_input_prediction(record, player_info);
    }

    mv.m_velocity = record->vel - mv.m_base_velocity;

    path.clear();
    mv.m_position = find_unstuck(record->origin);
    path.push_back(mv.m_position);

    return true;
}

void c_movement_simulate::air_input_prediction(const std::shared_ptr<player_record_t>& record,
                                               const player_t& player_info)
{
    mv.m_ground_dir = 0.f;
    mv.m_air_dir = estimate_air_dir(record->vel - mv.m_base_velocity, player_info.m_records[1]->vel, record->eye_angle,
                                    find_unstuck(player_info.m_records[1]->origin));

    mv.m_decay = 0.f;

    // if (mv.m_air_dir.length_2d() > 0.1f)
    //     mv.m_walk_direction = mv.m_air_dir;
}

void c_movement_simulate::ground_input_prediction(const player_t& player_info,
                                                  const std::shared_ptr<player_record_t>& record)
{
    mv.m_velocity = player_info.m_records[1]->vel;
    mv.m_velocity -= mv.m_base_velocity;
    mv.m_surface_friction = 1.f;
    for (auto i = 0; i < record->m_lag; i++)
        friction();
    vector last_fric_vel = mv.m_velocity;
    auto dif = (record->vel - mv.m_base_velocity) - last_fric_vel;
    const auto main_dif = dif;
    mv.m_air_dir = mv.m_walk_direction =
        estimate_walking_dir(record->vel - mv.m_base_velocity, last_fric_vel, record->eye_angle,
                             find_unstuck(player_info.m_records[1]->origin));
    
    float initial_angle = (rad_to_deg(atan2(mv.m_walk_direction.m_y, mv.m_walk_direction.m_x)));
    while (initial_angle > 180.f)
        initial_angle -= 360.f;
    while (initial_angle < -180.f)
        initial_angle += 360.f;

    bool rotation_timing_found = false;
    size_t count = std::min<size_t>(player_info.m_records.size() - 1, 133);
    float total_accumulated_angle_change = 0.f;
    float last_angle = initial_angle;
    float initial_angle_time = player_info.m_records[0]->sim_time;
    float total_circle_time = -1.f;
    bool disable_circle = false;
    float total_angle = initial_angle;
    float x = cos(deg_to_rad(initial_angle));
    float y = sin(deg_to_rad(initial_angle));
    float len = 1.0;

    for (int i = 1; i < count - 1; i++)
    {
        auto current_tick = player_info.m_records[i];
        auto tick_before = player_info.m_records[i + 1];

        if (!(current_tick->flags & 1) || !(tick_before->flags & 1))
            break;

        vector temp_velocity = mv.m_velocity;
        mv.m_velocity = tick_before->vel;
        friction();
        vector velocity_difference = current_tick->vel - mv.m_velocity;
        mv.m_velocity = temp_velocity;
        float speed_delta = velocity_difference.length_2d();

        if (speed_delta <= 1.f)
            continue;

        float new_angle = rad_to_deg(atan2(velocity_difference.m_y, velocity_difference.m_x));
        while (new_angle > 180.f)
            new_angle -= 360.f;
        while (new_angle < -180.f)
            new_angle += 360.f;

        float angle_diff = last_angle - new_angle;
        while (angle_diff > 180.f)
            angle_diff -= 360.f;
        while (angle_diff < -180.f)
            angle_diff += 360.f;

        last_angle = new_angle;
        float delta_to_inital_time = initial_angle_time - current_tick->sim_time;
        if (delta_to_inital_time < 0.4f)
        {
            x += cos(deg_to_rad(last_angle));
            y += sin(deg_to_rad(last_angle));
            len += 1.0;
        }
    }
    last_angle = initial_angle;
    for (int i = 1; i < count - 1; i++)
    {
        auto current_tick = player_info.m_records[i];
        auto tick_before = player_info.m_records[i + 1];

        if (!(current_tick->flags & 1) || !(tick_before->flags & 1))
            break;

        vector temp_velocity = mv.m_velocity;
        mv.m_velocity = tick_before->vel;
        friction();
        vector velocity_difference = current_tick->vel - mv.m_velocity;
        mv.m_velocity = temp_velocity;
        float speed_delta = velocity_difference.length_2d();

        if (speed_delta <= 1.f)
            continue;

        float new_angle = rad_to_deg(atan2(velocity_difference.m_y, velocity_difference.m_x));
        while (new_angle > 180.f)
            new_angle -= 360.f;
        while (new_angle < -180.f)
            new_angle += 360.f;

        float angle_diff = last_angle - new_angle;
        while (angle_diff > 180.f)
            angle_diff -= 360.f;
        while (angle_diff < -180.f)
            angle_diff += 360.f;
        total_accumulated_angle_change += angle_diff;

        last_angle = new_angle;
        float delta_to_inital_time = initial_angle_time - current_tick->sim_time;
        if (std::abs(total_accumulated_angle_change) < 90 && delta_to_inital_time > .5f)
            disable_circle = true;
        [[unlikely]] if (std::abs(total_accumulated_angle_change) > 500.f && !disable_circle)
        {
            total_circle_time = initial_angle_time - current_tick->sim_time;
            break;
        }
        if (rotation_timing_found)
        {
            if (disable_circle)
                break;
            continue;
        }

        float diff_from_inital_angle = initial_angle - new_angle;
        while (diff_from_inital_angle > 180.f)
            diff_from_inital_angle -= 360.f;
        while (diff_from_inital_angle < -180.f)
            diff_from_inital_angle += 360.f;
        if (std::abs(diff_from_inital_angle) < 45.f)
            continue;

        mv.ground_rotating_right = diff_from_inital_angle > 0.f;
        mv.ground_rotation_timing = delta_to_inital_time;
        rotation_timing_found = true;
    }
    [[unlikely]] if (total_circle_time > 0.f)
    {
        mv.ground_circle_prediction = true;
        mv.ground_rotating_right = total_accumulated_angle_change > 0.f;
        mv.ground_rotation_timing = total_circle_time;
    }
    else if (mv.m_walk_direction.length_2d() > 0.1f)
    {
        float wish_dir_angle = rad_to_deg(atan2(y, x));
        //wish_dir_angle += total_angle / end_count;
        mv.m_walk_direction.m_x = cos(deg_to_rad(wish_dir_angle)) * 450.f;
        mv.m_walk_direction.m_y = sin(deg_to_rad(wish_dir_angle)) * 450.f;
        mv.m_walk_direction.m_z = 0.f;
    }
}

void c_movement_simulate::do_angle_prediction(const std::shared_ptr<player_record_t>& record,
                                              const player_t& player_info)
{
    mv.m_dir = record->dir;
    return;
    int new_count = 1;
    const int count = fmin(player_info.m_records.size(), int(g_ui.m_controls.aim.players.interp->m_value + 1));
    float last_dir = record->dir;
    for (auto i = 1; i < count; i++)
    {
        mv.m_dir += player_info.m_records[i]->dir;
        new_count++;
    }
    mv.m_dir /= static_cast<float>(new_count);
}

void c_movement_simulate::draw()
{
    for (auto& i : m_logs)
    {
        auto& log = i.second;
        if (log.end_time <= 0)
            log.m_path.clear();

        if (log.m_path.size() < 2u)
            continue;

        float temp_time = log.end_time;
        int iter = 1;
        vector screen_1, screen_2, screen_3;
        float backup = g_ui.m_theme.m_a;
        g_ui.m_theme.m_a = 255;
        for (auto i = log.m_path.end() - 2; i != log.m_path.begin(); --i)
        {
            auto last = *(i + 1);
            if (temp_time < 0.f)
                break;

            if (math::world_to_screen(*(i + 1), screen_1) && math::world_to_screen(*i, screen_2))
            {
                c_render::line(screen_1, screen_2,
                               color(0x60, 0xf3, 0x21, 100 * fminf(temp_time, 1.f))); // 0xAE, 0xBA, 0xF8
            }
            if ((iter % 4) == 0)
            {
                const vector delta = (last - *i);
                vector angle = delta.angle_to();
                vector right;
                angle.angle_vectors(nullptr, &right, nullptr);
                right.m_z = 0.f;
                if (math::world_to_screen(last + right * fminf(200, 200 * (delta.length_2d()) / 300.f), screen_3) &&
                    math::world_to_screen(last, screen_1))
                {

                    c_render::line(screen_1, screen_3,
                                   color(0x60, 0xf3, 0x21, 100 * fminf(temp_time, 1.f))); // 0xAE, 0xBA, 0xF8
                }
                last = *i;
            }
            temp_time -= g_interfaces.m_global_vars->m_interval_per_tick;
            iter++;
        }
        g_ui.m_theme.m_a = backup;
        log.end_time -= g_interfaces.m_global_vars->m_frame_time;
    }
}

void c_movement_simulate::update_strafe()
{
    mv.m_eye_dir += mv.m_dir;
    mv.m_dir -= mv.m_dir * mv.m_decay;

    const double offsetValue = mv.m_eye_dir + 180.f; // value relative to 0

    mv.m_eye_dir = (offsetValue - (floor(offsetValue / 360.f) * 360.f)) - 180.f;
}

bool c_movement_simulate::run(vector& position)
{
    if (!mv.m_player)
        return false;

    mv.m_surface_friction = 1.f;

    update_strafe();
    player_move();

    path.push_back(mv.m_position);
    mv.m_time += g_interfaces.m_global_vars->m_interval_per_tick;
    end_time = mv.m_time + 10.f;

    position = mv.m_position;

    return true;
}
