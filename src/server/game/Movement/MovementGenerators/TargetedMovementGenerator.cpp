/*
 * Copyright (C) 2008-2012 TrinityCore <http://www.trinitycore.org/>
 * Copyright (C) 2005-2009 MaNGOS <http://getmangos.com/>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "ByteBuffer.h"
#include "TargetedMovementGenerator.h"
#include "Errors.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "World.h"
#include "MoveSplineInit.h"
#include "MoveSpline.h"
#include "Player.h"

#include <cmath>


template<class T, typename D>
void TargetedMovementGeneratorMedium<T,D>::_setTargetLocation(T* owner, bool updateDestination)
{
    if (!i_target.isValid() || !i_target->IsInWorld())
        return;

    if (owner->HasUnitState(UNIT_STATE_NOT_MOVE))
        return;

    // This is hacky. ToDo: remove.
    if (owner->GetTypeId() == TYPEID_UNIT)
    {
        switch(owner->GetEntry())
        {
            case 52498: // Beth'tilac.
                if (owner->GetMapId() == 720 && owner->GetAI()->GetData(2) == 0 && (i_target->GetPositionZ() < 100.0f || i_target->IsPetGuardianStuff()))
                    return;
                break;
            case 52581: // Cinderweb Drone.
            case 52447: // Cinderweb Spiderling.
            case 53745: // Engorged Broodling.
                if (owner->GetMapId() == 720)
                    if (i_target->GetPositionZ() > 100.0f)
                        return;
                break;
            case 56923: // Twilight Sapper
                if (owner->GetMotionMaster()->GetMotionSlot(MOTION_SLOT_CONTROLLED))
                    return;
                break;
            case 45870:
            case 45871:
            case 45872:
            case 45812:
                if (Creature* creature = owner->ToCreature())
                    if (creature->GetHomePosition().GetExactDist2d(i_target->GetPositionX(), i_target->GetPositionY()) > 60.0f)
                        return;
                break;
        }
    }

    float x, y, z;

    if (updateDestination || !i_path)
    {
        if (!i_offset)
        {
            if (i_target->IsWithinMeleeRange(owner))
                return;

            // To nearest random contact position.
            i_target->GetRandomContactPoint(owner, x, y, z, 0, MELEE_RANGE - 0.5f);
        }
        else
        {
            float dist;
            float size;

            // Pets need special handling.
            // We need to subtract GetObjectSize() because it gets added back further down the chain
            //  and that makes pets too far away. Subtracting it allows pets to properly
            //  be (GetCombatReach() + i_offset) away.
            // Only applies when i_target is pet's owner otherwise pets and mobs end up
            //   doing a "dance" while fighting
            if (owner->isPet() && i_target->GetTypeId() == TYPEID_PLAYER)
            {
                dist = 1.0f; //i_target->GetCombatReach();
                size = 1.0f; //i_target->GetCombatReach() - i_target->GetObjectSize();
            }
            else
            {
                dist = i_offset + 1.0f;
                size = owner->GetObjectSize();
            }

            if (i_target->IsWithinDistInMap(owner, dist))
                return;

            // To at i_offset distance from target and i_angle from target facing.
            i_target->GetClosePoint(x, y, z, size, i_offset, i_angle);
        }
    }
    else
    {
        // the destination has not changed, we just need to refresh the path (usually speed change)
        G3D::Vector3 end = i_path->GetEndPosition();
        x = end.x;
        y = end.y;
        z = end.z;
    }

    if (!i_path)
        i_path = new PathGenerator(owner);

    // allow pets to use shortcut if no path found when following their master
    bool forceDest = (owner->GetTypeId() == TYPEID_UNIT && owner->ToCreature()->isPet()
        && owner->HasUnitState(UNIT_STATE_FOLLOW));

    bool result = i_path->CalculatePath(x, y, z, forceDest);
    if (!result || (i_path->GetPathType() & PATHFIND_NOPATH))
    {
        // can't reach target
        i_recalculateTravel = true;
        return;
    }


    D::_addUnitStateMove(owner);
    i_targetReached = false;
    i_recalculateTravel = false;

    owner->UpdateAllowedPositionZ(x, y, z);

    Movement::MoveSplineInit init(owner);
    init.MovebyPath(i_path->GetPath());
    init.SetWalk(((D*)this)->EnableWalking());
    // Using the same condition for facing target as the one that is used for SetInFront on movement end - applies to ChaseMovementGenerator mostly.
    if (i_angle == 0.f)
        init.SetFacing(i_target.getTarget());
    init.Launch();
}

template<>
void TargetedMovementGeneratorMedium<Player,ChaseMovementGenerator<Player> >::UpdateFinalDistance(float /*fDistance*/)
{
    // Nothing to do for Player.
}

template<>
void TargetedMovementGeneratorMedium<Player,FollowMovementGenerator<Player> >::UpdateFinalDistance(float /*fDistance*/)
{
    // Nothing to do for Player.
}

template<>
void TargetedMovementGeneratorMedium<Creature,ChaseMovementGenerator<Creature> >::UpdateFinalDistance(float fDistance)
{
    i_offset = fDistance;
    i_recalculateTravel = true;
}

template<>
void TargetedMovementGeneratorMedium<Creature,FollowMovementGenerator<Creature> >::UpdateFinalDistance(float fDistance)
{
    i_offset = fDistance;
    i_recalculateTravel = true;
}

template<class T, typename D>
bool TargetedMovementGeneratorMedium<T,D>::DoUpdate(T* owner, uint32 diff)
{
    if (!i_target.isValid() || !i_target->IsInWorld())
        return false;

    if (!owner)
        return false;

    if (!owner->IsAlive())
        return false;

    if (owner->HasUnitState(UNIT_STATE_NOT_MOVE))
    {
        D::_clearUnitStateMove(owner);
        return true;
    }

    // Prevent movement while casting spells with cast time or channel time. Some creatures need adding here.
    if (owner->HasUnitState(UNIT_STATE_CASTING))
    {
        bool glyphwaterelemn = false;

        if (owner->GetOwner() && owner->GetOwner()->HasAura(63090) && owner->GetCharmInfo() && owner->GetCharmInfo()->HasCommandState(COMMAND_FOLLOW) && owner->ToPet() && owner->ToPet()->HasReactState(REACT_HELPER))
            glyphwaterelemn = true;

        if (!glyphwaterelemn)
        {
            if (!owner->IsStopped())
                owner->StopMoving();

            return true;
        }
    }

    // Prevent crash after creature killed pet.
    if (static_cast<D*>(this)->_lostTarget(owner))
    {
        D::_clearUnitStateMove(owner);
        return true;
    }

    bool targetMoved = false;
    i_recheckDistance.Update(diff);
    if (i_recheckDistance.Passed())
    {
        i_recheckDistance.Reset(100);

        // More distance let have better performance, less distance let have more sensitive reaction at target move.
        float allowed_dist = 0.0f;

        if (owner->isPet() && (owner->GetCharmerOrOwnerGUID() == i_target->GetGUID()))
            allowed_dist = 1.0f; // pet following owner
        else
            allowed_dist = owner->GetCombatReach() + sWorld->getRate(RATE_TARGET_POS_RECALCULATION_RANGE);

        G3D::Vector3 dest = owner->movespline->FinalDestination();
        if (owner->movespline->onTransport)
        {
            if (TransportBase* transport = owner->GetDirectTransport())
            {
                float o = owner->GetOrientation();
                transport->CalculatePassengerPosition(dest.x, dest.y, dest.z, o);
            }
        }

        // First check distance
        if (owner->GetTypeId() == TYPEID_UNIT && (owner->ToCreature()->CanFly() || owner->ToCreature()->canSwim()))
            targetMoved = !i_target->IsWithinDist3d(dest.x, dest.y, dest.z, allowed_dist);
        else
            targetMoved = !i_target->IsWithinDist2d(dest.x, dest.y, allowed_dist);

        // then, if the target is in range, check also Line of Sight.
        if (!targetMoved)
            targetMoved = !i_target->IsWithinLOSInMap(owner);
    }

    if (i_recalculateTravel || targetMoved)
        _setTargetLocation(owner, targetMoved);

    if (owner->movespline->Finalized())
    {
        static_cast<D*>(this)->MovementInform(owner);
        if (i_angle == 0.f && !owner->HasInArc(0.01f, i_target.getTarget()))
            owner->SetInFront(i_target.getTarget());

        if (!i_targetReached)
        {
            i_targetReached = true;
            static_cast<D*>(this)->_reachTarget(owner);
        }
    }

    return true;
}

// ========== ChaseMovementGenerator ============ //
template<class T>
void ChaseMovementGenerator<T>::_reachTarget(T* owner)
{
    if (!owner)
        return;

    if (owner->IsWithinMeleeRange(this->i_target.getTarget()))
        owner->Attack(this->i_target.getTarget(), true);
}

template<>
void ChaseMovementGenerator<Player>::DoInitialize(Player* owner)
{
    if (!owner)
        return;

    if (!owner->IsAlive())
        return;

    owner->AddUnitState(UNIT_STATE_CHASE | UNIT_STATE_CHASE_MOVE);
    _setTargetLocation(owner, true);
}

template<>
void ChaseMovementGenerator<Creature>::DoInitialize(Creature* owner)
{
    if (!owner)
        return;

    if (!owner->IsAlive())
        return;

    owner->SetWalk(false);
    owner->AddUnitState(UNIT_STATE_CHASE | UNIT_STATE_CHASE_MOVE);
    _setTargetLocation(owner, true);
}

template<class T>
void ChaseMovementGenerator<T>::DoReset(T* owner)
{
    DoInitialize(owner);
}

template<class T>
void ChaseMovementGenerator<T>::DoFinalize(T* owner)
{
    if (!owner)
        return;

    owner->ClearUnitState(UNIT_STATE_CHASE | UNIT_STATE_CHASE_MOVE);
}

template<class T>
void ChaseMovementGenerator<T>::MovementInform(T* /*owner*/) { }

template<>
void ChaseMovementGenerator<Creature>::MovementInform(Creature* owner)
{
    if (!owner)
        return;

    // Pass back the GUIDLow of the target. If it is pet's owner then PetAI will handle.
    if (owner->AI())
        owner->AI()->MovementInform(CHASE_MOTION_TYPE, i_target.getTarget()->GetGUIDLow());
}

// ========== FollowMovementGenerator ============ //

template<>
bool FollowMovementGenerator<Creature>::EnableWalking() const
{
    return i_target.isValid() && i_target->IsWalking();
}

template<>
bool FollowMovementGenerator<Player>::EnableWalking() const
{
    return false;
}

template<>
void FollowMovementGenerator<Player>::_updateSpeed(Player* /*owner*/)
{
    // Nothing to do for Player.
}

template<>
void FollowMovementGenerator<Creature>::_updateSpeed(Creature* owner)
{
    if (!owner)
        return;

    if (!owner->IsAlive() || !owner->IsInWorld())
        return;

    // Pet only syncs speed with owner.
    if (!owner->isPet() || !i_target.isValid())
        return;

    if (i_target->GetGUID() != owner->GetOwnerGUID())
        return;

    owner->UpdateSpeed(MOVE_RUN,true);
    owner->UpdateSpeed(MOVE_WALK,true);
    owner->UpdateSpeed(MOVE_SWIM,true);
}

template<>
void FollowMovementGenerator<Player>::DoInitialize(Player* owner)
{
    if (!owner)
        return;

    if (!owner->IsAlive())
        return;

    owner->AddUnitState(UNIT_STATE_FOLLOW | UNIT_STATE_FOLLOW_MOVE);

    _updateSpeed(owner);
    _setTargetLocation(owner, true);
}

template<>
void FollowMovementGenerator<Creature>::DoInitialize(Creature* owner)
{
    if (!owner)
        return;

    if (!owner->IsAlive())
        return;

    owner->AddUnitState(UNIT_STATE_FOLLOW | UNIT_STATE_FOLLOW_MOVE);

    _updateSpeed(owner);
    _setTargetLocation(owner, true);
}

template<class T>
void FollowMovementGenerator<T>::DoReset(T* owner)
{
    DoInitialize(owner);
}

template<class T>
void FollowMovementGenerator<T>::DoFinalize(T* owner)
{
    if (!owner)
        return;

    owner->ClearUnitState(UNIT_STATE_FOLLOW | UNIT_STATE_FOLLOW_MOVE);
    _updateSpeed(owner);
}

template<class T>
void FollowMovementGenerator<T>::MovementInform(T* /*owner*/) { }

template<>
void FollowMovementGenerator<Creature>::MovementInform(Creature* owner)
{
    if (!owner)
        return;

    // Pass back the GUIDLow of the target. If it is pet's owner then PetAI will handle
    if (owner->AI())
        owner->AI()->MovementInform(FOLLOW_MOTION_TYPE, i_target.getTarget()->GetGUIDLow());
}

template void TargetedMovementGeneratorMedium<Player, ChaseMovementGenerator<Player> >::_setTargetLocation(Player* owner, bool);
template void TargetedMovementGeneratorMedium<Player, FollowMovementGenerator<Player> >::_setTargetLocation(Player* owner, bool);
template void TargetedMovementGeneratorMedium<Creature, ChaseMovementGenerator<Creature> >::_setTargetLocation(Creature* owner, bool);
template void TargetedMovementGeneratorMedium<Creature, FollowMovementGenerator<Creature> >::_setTargetLocation(Creature* owner, bool);

template bool TargetedMovementGeneratorMedium<Player, ChaseMovementGenerator<Player> >::DoUpdate(Player* owner, uint32 diff);
template bool TargetedMovementGeneratorMedium<Player, FollowMovementGenerator<Player> >::DoUpdate(Player* owner, uint32 diff);
template bool TargetedMovementGeneratorMedium<Creature, ChaseMovementGenerator<Creature> >::DoUpdate(Creature* owner, uint32 diff);
template bool TargetedMovementGeneratorMedium<Creature, FollowMovementGenerator<Creature> >::DoUpdate(Creature* owner, uint32 diff);

template void ChaseMovementGenerator<Player>::_reachTarget(Player* owner);
template void ChaseMovementGenerator<Creature>::_reachTarget(Creature* owner);
template void ChaseMovementGenerator<Player>::DoFinalize(Player* owner);
template void ChaseMovementGenerator<Creature>::DoFinalize(Creature* owner);
template void ChaseMovementGenerator<Player>::DoReset(Player* owner);
template void ChaseMovementGenerator<Creature>::DoReset(Creature* owner);
template void ChaseMovementGenerator<Player>::MovementInform(Player* owner);

template void FollowMovementGenerator<Player>::DoFinalize(Player* owner);
template void FollowMovementGenerator<Creature>::DoFinalize(Creature* owner);
template void FollowMovementGenerator<Player>::DoReset(Player* owner);
template void FollowMovementGenerator<Creature>::DoReset(Creature* owner);
template void FollowMovementGenerator<Player>::MovementInform(Player* owner);
