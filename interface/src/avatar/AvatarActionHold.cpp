//
//  AvatarActionHold.cpp
//  interface/src/avatar/
//
//  Created by Seth Alves 2015-6-9
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "AvatarActionHold.h"

#include <QVariantGLM.h>

#include "avatar/AvatarManager.h"

const uint16_t AvatarActionHold::holdVersion = 1;

AvatarActionHold::AvatarActionHold(const QUuid& id, EntityItemPointer ownerEntity) :
    ObjectActionSpring(id, ownerEntity)
{
    _type = ACTION_TYPE_HOLD;
#if WANT_DEBUG
    qDebug() << "AvatarActionHold::AvatarActionHold";
#endif
}

AvatarActionHold::~AvatarActionHold() {
#if WANT_DEBUG
    qDebug() << "AvatarActionHold::~AvatarActionHold";
#endif
}

std::shared_ptr<Avatar> AvatarActionHold::getTarget(glm::quat& rotation, glm::vec3& position) {
    auto avatarManager = DependencyManager::get<AvatarManager>();
    auto holdingAvatar = std::static_pointer_cast<Avatar>(avatarManager->getAvatarBySessionID(_holderID));

    if (!holdingAvatar) {
        return holdingAvatar;
    }

    withTryReadLock([&]{
        bool isRightHand = (_hand == "right");
        glm::vec3 palmPosition { Vectors::ZERO };
        glm::quat palmRotation { Quaternions::IDENTITY };
            
        if (_ignoreIK && holdingAvatar->isMyAvatar()) {
            // We cannot ignore other avatars IK and this is not the point of this option
            // This is meant to make the grabbing behavior more reactive.
            if (isRightHand) {
                palmPosition = holdingAvatar->getHand()->getCopyOfPalmData(HandData::RightHand).getPosition();
                palmRotation = holdingAvatar->getHand()->getCopyOfPalmData(HandData::RightHand).getRotation();
            } else {
                palmPosition = holdingAvatar->getHand()->getCopyOfPalmData(HandData::LeftHand).getPosition();
                palmRotation = holdingAvatar->getHand()->getCopyOfPalmData(HandData::LeftHand).getRotation();
            }
        } else {
            if (isRightHand) {
                palmPosition = holdingAvatar->getRightPalmPosition();
                palmRotation = holdingAvatar->getRightPalmRotation();
            } else {
                palmPosition = holdingAvatar->getLeftPalmPosition();
                palmRotation = holdingAvatar->getLeftPalmRotation();
            }
        }

        rotation = palmRotation * _relativeRotation;
        position = palmPosition + rotation * _relativePosition;
    });

    return holdingAvatar;
}

void AvatarActionHold::updateActionWorker(float deltaTimeStep) {
    glm::quat rotation { Quaternions::IDENTITY };
    glm::vec3 position { Vectors::ZERO };
    bool valid = false;
    int holdCount = 0;

    auto ownerEntity = _ownerEntity.lock();
    if (!ownerEntity) {
        return;
    }
    QList<EntityActionPointer> holdActions = ownerEntity->getActionsOfType(ACTION_TYPE_HOLD);
    foreach (EntityActionPointer action, holdActions) {
        std::shared_ptr<AvatarActionHold> holdAction = std::static_pointer_cast<AvatarActionHold>(action);
        glm::quat rotationForAction;
        glm::vec3 positionForAction;
        std::shared_ptr<Avatar> holdingAvatar = holdAction->getTarget(rotationForAction, positionForAction);
        if (holdingAvatar) {
            holdCount ++;
            if (holdAction.get() == this) {
                // only use the rotation for this action
                valid = true;
                rotation = rotationForAction;
            }

            position += positionForAction;
        }
    }

    if (valid && holdCount > 0) {
        position /= holdCount;

        bool gotLock = withTryWriteLock([&]{
            _positionalTarget = position;
            _rotationalTarget = rotation;
            _positionalTargetSet = true;
            _rotationalTargetSet = true;
            _active = true;
        });
        if (gotLock) {
            if (_kinematic) {
                doKinematicUpdate(deltaTimeStep);
            } else {
                activateBody();
                forceBodyNonStatic();
                ObjectActionSpring::updateActionWorker(deltaTimeStep);
            }
        }
    }
}

void AvatarActionHold::doKinematicUpdate(float deltaTimeStep) {
    auto ownerEntity = _ownerEntity.lock();
    if (!ownerEntity) {
        qDebug() << "AvatarActionHold::doKinematicUpdate -- no owning entity";
        return;
    }
    void* physicsInfo = ownerEntity->getPhysicsInfo();
    if (!physicsInfo) {
        qDebug() << "AvatarActionHold::doKinematicUpdate -- no owning physics info";
        return;
    }
    ObjectMotionState* motionState = static_cast<ObjectMotionState*>(physicsInfo);
    btRigidBody* rigidBody = motionState ? motionState->getRigidBody() : nullptr;
    if (!rigidBody) {
        qDebug() << "AvatarActionHold::doKinematicUpdate -- no rigidBody";
        return;
    }

    withWriteLock([&]{
        if (_kinematicSetVelocity) {
            if (_previousSet) {
                // smooth velocity over 2 frames
                glm::vec3 positionalDelta = _positionalTarget - _previousPositionalTarget;
                glm::vec3 positionalVelocity =
                    (positionalDelta + _previousPositionalDelta) / (deltaTimeStep + _previousDeltaTimeStep);
                rigidBody->setLinearVelocity(glmToBullet(positionalVelocity));
                _previousPositionalDelta = positionalDelta;
                _previousDeltaTimeStep = deltaTimeStep;
            }
        }

        btTransform worldTrans = rigidBody->getWorldTransform();
        worldTrans.setOrigin(glmToBullet(_positionalTarget));
        worldTrans.setRotation(glmToBullet(_rotationalTarget));
        rigidBody->setWorldTransform(worldTrans);

        motionState->dirtyInternalKinematicChanges();

        _previousPositionalTarget = _positionalTarget;
        _previousRotationalTarget = _rotationalTarget;
        _previousSet = true;
    });

    activateBody();
    forceBodyNonStatic();
}

bool AvatarActionHold::updateArguments(QVariantMap arguments) {
    glm::vec3 relativePosition;
    glm::quat relativeRotation;
    float timeScale;
    QString hand;
    QUuid holderID;
    bool kinematic;
    bool kinematicSetVelocity;
    bool ignoreIK;
    bool needUpdate = false;

    bool somethingChanged = ObjectAction::updateArguments(arguments);
    withReadLock([&]{
        bool ok = true;
        relativePosition = EntityActionInterface::extractVec3Argument("hold", arguments, "relativePosition", ok, false);
        if (!ok) {
            relativePosition = _relativePosition;
        }

        ok = true;
        relativeRotation = EntityActionInterface::extractQuatArgument("hold", arguments, "relativeRotation", ok, false);
        if (!ok) {
            relativeRotation = _relativeRotation;
        }

        ok = true;
        timeScale = EntityActionInterface::extractFloatArgument("hold", arguments, "timeScale", ok, false);
        if (!ok) {
            timeScale = _linearTimeScale;
        }

        ok = true;
        hand = EntityActionInterface::extractStringArgument("hold", arguments, "hand", ok, false);
        if (!ok || !(hand == "left" || hand == "right")) {
            hand = _hand;
        }

        ok = true;
        auto myAvatar = DependencyManager::get<AvatarManager>()->getMyAvatar();
        holderID = myAvatar->getSessionUUID();

        ok = true;
        kinematic = EntityActionInterface::extractBooleanArgument("hold", arguments, "kinematic", ok, false);
        if (!ok) {
            kinematic = _kinematic;
        }

        ok = true;
        kinematicSetVelocity = EntityActionInterface::extractBooleanArgument("hold", arguments,
                                                                             "kinematicSetVelocity", ok, false);
        if (!ok) {
            kinematicSetVelocity = _kinematicSetVelocity;
        }

        ok = true;
        ignoreIK = EntityActionInterface::extractBooleanArgument("hold", arguments, "ignoreIK", ok, false);
        if (!ok) {
            ignoreIK = _ignoreIK;
        }

        if (somethingChanged ||
            relativePosition != _relativePosition ||
            relativeRotation != _relativeRotation ||
            timeScale != _linearTimeScale ||
            hand != _hand ||
            holderID != _holderID ||
            kinematic != _kinematic ||
            kinematicSetVelocity != _kinematicSetVelocity ||
            ignoreIK != _ignoreIK) {
            needUpdate = true;
        }
    });

    if (needUpdate) {
        withWriteLock([&] {
            _relativePosition = relativePosition;
            _relativeRotation = relativeRotation;
            const float MIN_TIMESCALE = 0.1f;
            _linearTimeScale = glm::max(MIN_TIMESCALE, timeScale);
            _angularTimeScale = _linearTimeScale;
            _hand = hand;
            _holderID = holderID;
            _kinematic = kinematic;
            _kinematicSetVelocity = kinematicSetVelocity;
            _ignoreIK = ignoreIK;
            _active = true;

            auto ownerEntity = _ownerEntity.lock();
            if (ownerEntity) {
                ownerEntity->setActionDataDirty(true);
                ownerEntity->setActionDataNeedsTransmit(true);
            }
        });
        activateBody();
    }

    return true;
}

QVariantMap AvatarActionHold::getArguments() {
    QVariantMap arguments = ObjectAction::getArguments();
    withReadLock([&]{
        arguments["holderID"] = _holderID;
        arguments["relativePosition"] = glmToQMap(_relativePosition);
        arguments["relativeRotation"] = glmToQMap(_relativeRotation);
        arguments["timeScale"] = _linearTimeScale;
        arguments["hand"] = _hand;
        arguments["kinematic"] = _kinematic;
        arguments["kinematicSetVelocity"] = _kinematicSetVelocity;
        arguments["ignoreIK"] = _ignoreIK;
    });
    return arguments;
}

QByteArray AvatarActionHold::serialize() const {
    QByteArray serializedActionArguments;
    QDataStream dataStream(&serializedActionArguments, QIODevice::WriteOnly);

    withReadLock([&]{
        dataStream << ACTION_TYPE_HOLD;
        dataStream << getID();
        dataStream << AvatarActionHold::holdVersion;

        dataStream << _holderID;
        dataStream << _relativePosition;
        dataStream << _relativeRotation;
        dataStream << _linearTimeScale;
        dataStream << _hand;

        dataStream << localTimeToServerTime(_expires);
        dataStream << _tag;
        dataStream << _kinematic;
        dataStream << _kinematicSetVelocity;
    });

    return serializedActionArguments;
}

void AvatarActionHold::deserialize(QByteArray serializedArguments) {
    QDataStream dataStream(serializedArguments);

    EntityActionType type;
    dataStream >> type;
    assert(type == getType());

    QUuid id;
    dataStream >> id;
    assert(id == getID());

    uint16_t serializationVersion;
    dataStream >> serializationVersion;
    if (serializationVersion != AvatarActionHold::holdVersion) {
        return;
    }

    withWriteLock([&]{
        dataStream >> _holderID;
        dataStream >> _relativePosition;
        dataStream >> _relativeRotation;
        dataStream >> _linearTimeScale;
        _angularTimeScale = _linearTimeScale;
        dataStream >> _hand;

        quint64 serverExpires;
        dataStream >> serverExpires;
        _expires = serverTimeToLocalTime(serverExpires);

        dataStream >> _tag;
        dataStream >> _kinematic;
        dataStream >> _kinematicSetVelocity;

        #if WANT_DEBUG
        qDebug() << "deserialize AvatarActionHold: " << _holderID
                 << _relativePosition.x << _relativePosition.y << _relativePosition.z
                 << _hand << _expires;
        #endif

        _active = true;
    });

    activateBody();
    forceBodyNonStatic();
}
