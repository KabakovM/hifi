//
//  AvatarManager.cpp
//  interface/src/avatar
//
//  Created by Stephen Birarda on 1/23/2014.
//  Copyright 2014 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include <string>

#include <QScriptEngine>

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#endif

#include <glm/gtx/string_cast.hpp>

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif


#include <PerfStat.h>
#include <RegisteredMetaTypes.h>
#include <Rig.h>
#include <SettingHandle.h>
#include <UUID.h>

#include "Application.h"
#include "Avatar.h"
#include "AvatarManager.h"
#include "Menu.h"
#include "MyAvatar.h"
#include "SceneScriptingInterface.h"

// 70 times per second - target is 60hz, but this helps account for any small deviations
// in the update loop
static const quint64 MIN_TIME_BETWEEN_MY_AVATAR_DATA_SENDS = (1000 * 1000) / 70;

// We add _myAvatar into the hash with all the other AvatarData, and we use the default NULL QUid as the key.
const QUuid MY_AVATAR_KEY;  // NULL key

static QScriptValue localLightToScriptValue(QScriptEngine* engine, const AvatarManager::LocalLight& light) {
    QScriptValue object = engine->newObject();
    object.setProperty("direction", vec3toScriptValue(engine, light.direction));
    object.setProperty("color", vec3toScriptValue(engine, light.color));
    return object;
}

static void localLightFromScriptValue(const QScriptValue& value, AvatarManager::LocalLight& light) {
    vec3FromScriptValue(value.property("direction"), light.direction);
    vec3FromScriptValue(value.property("color"), light.color);
}

void AvatarManager::registerMetaTypes(QScriptEngine* engine) {
    qScriptRegisterMetaType(engine, localLightToScriptValue, localLightFromScriptValue);
    qScriptRegisterSequenceMetaType<QVector<AvatarManager::LocalLight> >(engine);
}

AvatarManager::AvatarManager(QObject* parent) :
    _avatarFades()
{
    // register a meta type for the weak pointer we'll use for the owning avatar mixer for each avatar
    qRegisterMetaType<QWeakPointer<Node> >("NodeWeakPointer");
    _myAvatar = std::make_shared<MyAvatar>(std::make_shared<Rig>());

    auto& packetReceiver = DependencyManager::get<NodeList>()->getPacketReceiver();
    packetReceiver.registerListener(PacketType::BulkAvatarData, this, "processAvatarDataPacket");
    packetReceiver.registerListener(PacketType::KillAvatar, this, "processKillAvatar");
    packetReceiver.registerListener(PacketType::AvatarIdentity, this, "processAvatarIdentityPacket");
    packetReceiver.registerListener(PacketType::AvatarBillboard, this, "processAvatarBillboardPacket");
}

const float SMALLEST_REASONABLE_HORIZON = 5.0f; // meters
Setting::Handle<float> avatarRenderDistanceInverseHighLimit("avatarRenderDistanceHighLimit", 1.0f / SMALLEST_REASONABLE_HORIZON);
void AvatarManager::setRenderDistanceInverseHighLimit(float newValue) {
    avatarRenderDistanceInverseHighLimit.set(newValue);
     _renderDistanceController.setControlledValueHighLimit(newValue);
}

void AvatarManager::init() {
    _myAvatar->init();
    {
        QWriteLocker locker(&_hashLock);
        _avatarHash.insert(MY_AVATAR_KEY, _myAvatar);
    }

    connect(DependencyManager::get<SceneScriptingInterface>().data(), &SceneScriptingInterface::shouldRenderAvatarsChanged, this, &AvatarManager::updateAvatarRenderStatus, Qt::QueuedConnection);

    render::ScenePointer scene = qApp->getMain3DScene();
    render::PendingChanges pendingChanges;
    if (DependencyManager::get<SceneScriptingInterface>()->shouldRenderAvatars()) {
        _myAvatar->addToScene(_myAvatar, scene, pendingChanges);
    }
    scene->enqueuePendingChanges(pendingChanges);

    const float target_fps = qApp->getTargetFrameRate();
    _renderDistanceController.setMeasuredValueSetpoint(target_fps);
    _renderDistanceController.setControlledValueHighLimit(avatarRenderDistanceInverseHighLimit.get());
    _renderDistanceController.setControlledValueLowLimit(1.0f / (float) TREE_SCALE);
    // Advice for tuning parameters:
    // See PIDController.h. There's a section on tuning in the reference.
    // Turn on logging with the following (or from js with AvatarList.setRenderDistanceControllerHistory("avatar render", 300))
    //_renderDistanceController.setHistorySize("avatar render", target_fps * 4);
    // Note that extra logging/hysteresis is turned off in Avatar.cpp when the above logging is on.
    _renderDistanceController.setKP(0.0008f); // Usually about 0.6 of largest that doesn't oscillate when other parameters 0.
    _renderDistanceController.setKI(0.0006f); // Big enough to bring us to target with the above KP.
    _renderDistanceController.setKD(0.000001f); // A touch of kd increases the speed by which we get there.
}

void AvatarManager::updateMyAvatar(float deltaTime) {
    bool showWarnings = Menu::getInstance()->isOptionChecked(MenuOption::PipelineWarnings);
    PerformanceWarning warn(showWarnings, "AvatarManager::updateMyAvatar()");

    _myAvatar->update(deltaTime);

    quint64 now = usecTimestampNow();
    quint64 dt = now - _lastSendAvatarDataTime;

    if (dt > MIN_TIME_BETWEEN_MY_AVATAR_DATA_SENDS) {
        // send head/hand data to the avatar mixer and voxel server
        PerformanceTimer perfTimer("send");
        _myAvatar->sendAvatarDataPacket();
        _lastSendAvatarDataTime = now;
    }
}

void AvatarManager::updateOtherAvatars(float deltaTime) {
    // lock the hash for read to check the size
    QReadLocker lock(&_hashLock);
    
    if (_avatarHash.size() < 2 && _avatarFades.isEmpty()) {
        return;
    }
    
    lock.unlock();
    
    bool showWarnings = Menu::getInstance()->isOptionChecked(MenuOption::PipelineWarnings);
    PerformanceWarning warn(showWarnings, "Application::updateAvatars()");

    PerformanceTimer perfTimer("otherAvatars");
    
    float distance;
    if (!qApp->isThrottleRendering()) {
        _renderDistanceController.setMeasuredValueSetpoint(qApp->getTargetFrameRate()); // No problem updating in flight.
        // The PID controller raises the controlled value when the measured value goes up.
        // The measured value is frame rate. When the controlled value (1 / render cutoff distance)
        // goes up, the render cutoff distance gets closer, the number of rendered avatars is less, and frame rate
        // goes up.
        const float deduced = qApp->getLastUnsynchronizedFps();
        distance = 1.0f / _renderDistanceController.update(deduced, deltaTime);
    } else {
        // Here we choose to just use the maximum render cutoff distance if throttled.
        distance = 1.0f / _renderDistanceController.getControlledValueLowLimit();
    }
    _renderDistanceAverage.updateAverage(distance);
    _renderDistance = _renderDistanceAverage.getAverage();
    int renderableCount = 0;

    // simulate avatars
    auto hashCopy = getHashCopy();
    
    AvatarHash::iterator avatarIterator = hashCopy.begin();
    while (avatarIterator != hashCopy.end()) {
        auto avatar = std::static_pointer_cast<Avatar>(avatarIterator.value());

        if (avatar == _myAvatar || !avatar->isInitialized()) {
            // DO NOT update _myAvatar!  Its update has already been done earlier in the main loop.
            // DO NOT update or fade out uninitialized Avatars
            ++avatarIterator;
        } else if (avatar->shouldDie()) {
            removeAvatar(avatarIterator.key());
            ++avatarIterator;
        } else {
            avatar->startUpdate();
            avatar->simulate(deltaTime);
            if (avatar->getShouldRender()) {
                renderableCount++;
            }
            avatar->endUpdate();
            ++avatarIterator;
        }
    }
    _renderedAvatarCount = renderableCount;

    // simulate avatar fades
    simulateAvatarFades(deltaTime);
}

void AvatarManager::simulateAvatarFades(float deltaTime) {
    QVector<AvatarSharedPointer>::iterator fadingIterator = _avatarFades.begin();

    const float SHRINK_RATE = 0.9f;
    const float MIN_FADE_SCALE = MIN_AVATAR_SCALE;

    render::ScenePointer scene = qApp->getMain3DScene();
    render::PendingChanges pendingChanges;
    while (fadingIterator != _avatarFades.end()) {
        auto avatar = std::static_pointer_cast<Avatar>(*fadingIterator);
        avatar->startUpdate();
        avatar->setTargetScale(avatar->getAvatarScale() * SHRINK_RATE);
        if (avatar->getTargetScale() <= MIN_FADE_SCALE) {
            avatar->removeFromScene(*fadingIterator, scene, pendingChanges);
            fadingIterator = _avatarFades.erase(fadingIterator);
        } else {
            avatar->simulate(deltaTime);
            ++fadingIterator;
        }
        avatar->endUpdate();
    }
    scene->enqueuePendingChanges(pendingChanges);
}

AvatarSharedPointer AvatarManager::newSharedAvatar() {
    return std::make_shared<Avatar>(std::make_shared<Rig>());
}

AvatarSharedPointer AvatarManager::addAvatar(const QUuid& sessionUUID, const QWeakPointer<Node>& mixerWeakPointer) {
    auto newAvatar = AvatarHashMap::addAvatar(sessionUUID, mixerWeakPointer);
    auto rawRenderableAvatar = std::static_pointer_cast<Avatar>(newAvatar);
    
    render::ScenePointer scene = qApp->getMain3DScene();
    render::PendingChanges pendingChanges;
    if (DependencyManager::get<SceneScriptingInterface>()->shouldRenderAvatars()) {
        rawRenderableAvatar->addToScene(rawRenderableAvatar, scene, pendingChanges);
    }
    scene->enqueuePendingChanges(pendingChanges);
    
    return newAvatar;
}

// protected
void AvatarManager::removeAvatarMotionState(AvatarSharedPointer avatar) {
    auto rawPointer = std::static_pointer_cast<Avatar>(avatar);
    AvatarMotionState* motionState = rawPointer->getMotionState();
    if (motionState) {
        // clean up physics stuff
        motionState->clearObjectBackPointer();
        rawPointer->setMotionState(nullptr);
        _avatarMotionStates.remove(motionState);
        _motionStatesToAdd.remove(motionState);
        _motionStatesToDelete.push_back(motionState);
    }
}

// virtual
void AvatarManager::removeAvatar(const QUuid& sessionUUID) {
    QWriteLocker locker(&_hashLock);
    
    auto removedAvatar = _avatarHash.take(sessionUUID);
    if (removedAvatar) {
        handleRemovedAvatar(removedAvatar);
    }
}

void AvatarManager::handleRemovedAvatar(const AvatarSharedPointer& removedAvatar) {
    AvatarHashMap::handleRemovedAvatar(removedAvatar);
    
    removeAvatarMotionState(removedAvatar);
    _avatarFades.push_back(removedAvatar);
}

void AvatarManager::clearOtherAvatars() {
    // clear any avatars that came from an avatar-mixer
    QWriteLocker locker(&_hashLock);
    
    AvatarHash::iterator avatarIterator =  _avatarHash.begin();
    while (avatarIterator != _avatarHash.end()) {
        auto avatar = std::static_pointer_cast<Avatar>(avatarIterator.value());
        if (avatar == _myAvatar || !avatar->isInitialized()) {
            // don't remove myAvatar or uninitialized avatars from the list
            ++avatarIterator;
        } else {
            auto removedAvatar = avatarIterator.value();
            avatarIterator = _avatarHash.erase(avatarIterator);
            
            handleRemovedAvatar(removedAvatar);
        }
    }
    _myAvatar->clearLookAtTargetAvatar();
}

void AvatarManager::setLocalLights(const QVector<AvatarManager::LocalLight>& localLights) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, "setLocalLights", Q_ARG(const QVector<AvatarManager::LocalLight>&, localLights));
        return;
    }
    _localLights = localLights;
}

QVector<AvatarManager::LocalLight> AvatarManager::getLocalLights() const {
    if (QThread::currentThread() != thread()) {
        QVector<AvatarManager::LocalLight> result;
        QMetaObject::invokeMethod(const_cast<AvatarManager*>(this), "getLocalLights", Qt::BlockingQueuedConnection,
            Q_RETURN_ARG(QVector<AvatarManager::LocalLight>, result));
        return result;
    }
    return _localLights;
}

QVector<QUuid> AvatarManager::getAvatarIdentifiers() {
    QReadLocker locker(&_hashLock);
    return _avatarHash.keys().toVector();
}

AvatarData* AvatarManager::getAvatar(QUuid avatarID) {
    QReadLocker locker(&_hashLock);
    return _avatarHash[avatarID].get();  // Non-obvious: A bogus avatarID answers your own avatar.
}


void AvatarManager::getObjectsToDelete(VectorOfMotionStates& result) {
    result.clear();
    result.swap(_motionStatesToDelete);
}

void AvatarManager::getObjectsToAdd(VectorOfMotionStates& result) {
    result.clear();
    for (auto motionState : _motionStatesToAdd) {
        result.push_back(motionState);
    }
    _motionStatesToAdd.clear();
}

void AvatarManager::getObjectsToChange(VectorOfMotionStates& result) {
    result.clear();
    for (auto state : _avatarMotionStates) {
        if (state->_dirtyFlags > 0) {
            result.push_back(state);
        }
    }
}

void AvatarManager::handleOutgoingChanges(const VectorOfMotionStates& motionStates) {
    // TODO: extract the MyAvatar results once we use a MotionState for it.
}

void AvatarManager::handleCollisionEvents(const CollisionEvents& collisionEvents) {
    for (Collision collision : collisionEvents) {
        // TODO: The plan is to handle MOTIONSTATE_TYPE_AVATAR, and then MOTIONSTATE_TYPE_MYAVATAR. As it is, other
        // people's avatars will have an id that doesn't match any entities, and one's own avatar will have
        // an id of null. Thus this code handles any collision in which one of the participating objects is
        // my avatar. (Other user machines will make a similar analysis and inject sound for their collisions.)
        if (collision.idA.isNull() || collision.idB.isNull()) {
            MyAvatar* myAvatar = getMyAvatar();
            const QString& collisionSoundURL = myAvatar->getCollisionSoundURL();
            if (!collisionSoundURL.isEmpty()) {
                const float velocityChange = glm::length(collision.velocityChange);
                const float MIN_AVATAR_COLLISION_ACCELERATION = 0.01f;
                const bool isSound = (collision.type == CONTACT_EVENT_TYPE_START) && (velocityChange > MIN_AVATAR_COLLISION_ACCELERATION);

                if (!isSound) {
                    return;  // No sense iterating for others. We only have one avatar.
                }
                // Your avatar sound is personal to you, so let's say the "mass" part of the kinetic energy is already accounted for.
                const float energy = velocityChange * velocityChange;
                const float COLLISION_ENERGY_AT_FULL_VOLUME = 0.5f;
                const float energyFactorOfFull = fmin(1.0f, energy / COLLISION_ENERGY_AT_FULL_VOLUME);

                // For general entity collisionSoundURL, playSound supports changing the pitch for the sound based on the size of the object,
                // but most avatars are roughly the same size, so let's not be so fancy yet.
                const float AVATAR_STRETCH_FACTOR = 1.0f;

                AudioInjector::playSound(collisionSoundURL, energyFactorOfFull, AVATAR_STRETCH_FACTOR, myAvatar->getPosition());
                myAvatar->collisionWithEntity(collision);
                return;
            }
        }
    }
}

void AvatarManager::updateAvatarPhysicsShape(Avatar* avatar) {
    AvatarMotionState* motionState = avatar->getMotionState();
    if (motionState) {
        motionState->addDirtyFlags(Simulation::DIRTY_SHAPE);
    } else {
        ShapeInfo shapeInfo;
        avatar->computeShapeInfo(shapeInfo);
        btCollisionShape* shape = ObjectMotionState::getShapeManager()->getShape(shapeInfo);
        if (shape) {
            AvatarMotionState* motionState = new AvatarMotionState(avatar, shape);
            avatar->setMotionState(motionState);
            _motionStatesToAdd.insert(motionState);
            _avatarMotionStates.insert(motionState);
        }
    }
}

void AvatarManager::updateAvatarRenderStatus(bool shouldRenderAvatars) {
    if (DependencyManager::get<SceneScriptingInterface>()->shouldRenderAvatars()) {
        for (auto avatarData : _avatarHash) {
            auto avatar = std::static_pointer_cast<Avatar>(avatarData);
            render::ScenePointer scene = qApp->getMain3DScene();
            render::PendingChanges pendingChanges;
            avatar->addToScene(avatar, scene, pendingChanges);
            scene->enqueuePendingChanges(pendingChanges);
        }
    } else {
        for (auto avatarData : _avatarHash) {
            auto avatar = std::static_pointer_cast<Avatar>(avatarData);
            render::ScenePointer scene = qApp->getMain3DScene();
            render::PendingChanges pendingChanges;
            avatar->removeFromScene(avatar, scene, pendingChanges);
            scene->enqueuePendingChanges(pendingChanges);
        }
    }
}


AvatarSharedPointer AvatarManager::getAvatarBySessionID(const QUuid& sessionID) {
    if (sessionID == _myAvatar->getSessionUUID()) {
        return _myAvatar;
    }
    
    return findAvatar(sessionID);
}
