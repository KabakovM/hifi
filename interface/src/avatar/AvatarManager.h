//
//  AvatarManager.h
//  interface/src/avatar
//
//  Created by Stephen Birarda on 1/23/2014.
//  Copyright 2014 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_AvatarManager_h
#define hifi_AvatarManager_h

#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QSharedPointer>

#include <AvatarHashMap.h>
#include <PhysicsEngine.h>
#include <PIDController.h>
#include <SimpleMovingAverage.h>

#include "Avatar.h"
#include "AvatarMotionState.h"

class MyAvatar;

class AvatarManager : public AvatarHashMap {
    Q_OBJECT
    SINGLETON_DEPENDENCY

public:
    /// Registers the script types associated with the avatar manager.
    static void registerMetaTypes(QScriptEngine* engine);

    void init();

    MyAvatar* getMyAvatar() { return _myAvatar.get(); }
    AvatarSharedPointer getAvatarBySessionID(const QUuid& sessionID);

    void updateMyAvatar(float deltaTime);
    void updateOtherAvatars(float deltaTime);
    
    void clearOtherAvatars();
   
    bool shouldShowReceiveStats() const { return _shouldShowReceiveStats; }
    PIDController& getRenderDistanceController()  { return _renderDistanceController; }

    class LocalLight {
    public:
        glm::vec3 color;
        glm::vec3 direction;
    };
    
    Q_INVOKABLE void setLocalLights(const QVector<AvatarManager::LocalLight>& localLights);
    Q_INVOKABLE QVector<AvatarManager::LocalLight> getLocalLights() const;
    // Currently, your own avatar will be included as the null avatar id.
    Q_INVOKABLE QVector<QUuid> getAvatarIdentifiers();
    Q_INVOKABLE AvatarData* getAvatar(QUuid avatarID);


    void getObjectsToDelete(VectorOfMotionStates& motionStates);
    void getObjectsToAdd(VectorOfMotionStates& motionStates);
    void getObjectsToChange(VectorOfMotionStates& motionStates);
    void handleOutgoingChanges(const VectorOfMotionStates& motionStates);
    void handleCollisionEvents(const CollisionEvents& collisionEvents);

    void updateAvatarPhysicsShape(Avatar* avatar);
    
    // Expose results and parameter-tuning operations to other systems, such as stats and javascript.
    Q_INVOKABLE float getRenderDistance() { return _renderDistance; }
    Q_INVOKABLE float getRenderDistanceInverseLowLimit() { return _renderDistanceController.getControlledValueLowLimit(); }
    Q_INVOKABLE float getRenderDistanceInverseHighLimit() { return _renderDistanceController.getControlledValueHighLimit(); }
    Q_INVOKABLE int getNumberInRenderRange() { return _renderedAvatarCount; }
    Q_INVOKABLE bool getRenderDistanceControllerIsLogging() { return _renderDistanceController.getIsLogging(); }
    Q_INVOKABLE void setRenderDistanceControllerHistory(QString label, int size) { return _renderDistanceController.setHistorySize(label, size); }
    Q_INVOKABLE void setRenderDistanceKP(float newValue) { _renderDistanceController.setKP(newValue); }
    Q_INVOKABLE void setRenderDistanceKI(float newValue) { _renderDistanceController.setKI(newValue); }
    Q_INVOKABLE void setRenderDistanceKD(float newValue) { _renderDistanceController.setKD(newValue); }
    Q_INVOKABLE void setRenderDistanceInverseLowLimit(float newValue) { _renderDistanceController.setControlledValueLowLimit(newValue); }
    Q_INVOKABLE void setRenderDistanceInverseHighLimit(float newValue);
   
public slots:
    void setShouldShowReceiveStats(bool shouldShowReceiveStats) { _shouldShowReceiveStats = shouldShowReceiveStats; }
    void updateAvatarRenderStatus(bool shouldRenderAvatars);

private:
    AvatarManager(QObject* parent = 0);
    AvatarManager(const AvatarManager& other);

    void simulateAvatarFades(float deltaTime);
    
    // virtual overrides
    virtual AvatarSharedPointer newSharedAvatar();
    virtual AvatarSharedPointer addAvatar(const QUuid& sessionUUID, const QWeakPointer<Node>& mixerWeakPointer);
    void removeAvatarMotionState(AvatarSharedPointer avatar);
    
    virtual void removeAvatar(const QUuid& sessionUUID);
    virtual void handleRemovedAvatar(const AvatarSharedPointer& removedAvatar);
    
    QVector<AvatarSharedPointer> _avatarFades;
    std::shared_ptr<MyAvatar> _myAvatar;
    quint64 _lastSendAvatarDataTime = 0; // Controls MyAvatar send data rate.
    
    QVector<AvatarManager::LocalLight> _localLights;

    bool _shouldShowReceiveStats = false;
    float _renderDistance { (float) TREE_SCALE };
    int _renderedAvatarCount { 0 };
    PIDController _renderDistanceController { };
    SimpleMovingAverage _renderDistanceAverage { 10 };

    SetOfAvatarMotionStates _avatarMotionStates;
    SetOfMotionStates _motionStatesToAdd;
    VectorOfMotionStates _motionStatesToDelete;
};

Q_DECLARE_METATYPE(AvatarManager::LocalLight)
Q_DECLARE_METATYPE(QVector<AvatarManager::LocalLight>)

#endif // hifi_AvatarManager_h
