//
//  AvatarActionHold.h
//  interface/src/avatar/
//
//  Created by Seth Alves 2015-6-9
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_AvatarActionHold_h
#define hifi_AvatarActionHold_h

#include <QUuid>

#include <EntityItem.h>
#include <ObjectActionSpring.h>

#include "avatar/MyAvatar.h"


class AvatarActionHold : public ObjectActionSpring {
public:
    AvatarActionHold(const QUuid& id, EntityItemPointer ownerEntity);
    virtual ~AvatarActionHold();

    virtual bool updateArguments(QVariantMap arguments);
    virtual QVariantMap getArguments();

    virtual void updateActionWorker(float deltaTimeStep);

    QByteArray serialize() const;
    virtual void deserialize(QByteArray serializedArguments);

    virtual bool shouldSuppressLocationEdits() { return _active && !_ownerEntity.expired(); }

    std::shared_ptr<Avatar> getTarget(glm::quat& rotation, glm::vec3& position);

private:
    void doKinematicUpdate(float deltaTimeStep);

    static const uint16_t holdVersion;

    glm::vec3 _relativePosition { Vectors::ZERO };
    glm::quat _relativeRotation { Quaternions::IDENTITY };
    QString _hand { "right" };
    QUuid _holderID;

    bool _kinematic { false };
    bool _kinematicSetVelocity { false };
    bool _previousSet { false };
    bool _ignoreIK { false };
    glm::vec3 _previousPositionalTarget;
    glm::quat _previousRotationalTarget;

    float _previousDeltaTimeStep = 0.0f;
    glm::vec3 _previousPositionalDelta;
};

#endif // hifi_AvatarActionHold_h
