// -*- coding: utf-8 -*-
// Copyright (C) 2015 James Taylor, Rosen Diankov <rosen.diankov@gmail.com>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#ifndef OPENRAVE_MOBY_SPACE
#define OPENRAVE_MOBY_SPACE

#include "plugindefs.h"

#include <Moby/TimeSteppingSimulator.h>
#include <Moby/RCArticulatedBody.h>
#include <Moby/RigidBody.h>
#include <Moby/Joint.h>
#include <Moby/TriangleMeshPrimitive.h>
#include <Ravelin/MatrixNd.h>
#include <Ravelin/Vector3d.h>
#include <Ravelin/Pose3d.h>

#include <iostream>  // temp... bypass RAVELOG for now

// manages a space of Moby objects
// Note that if inheriting from Moby::Simulator then shared_from_this lies
//  at the bottom of the inheritence tree for Simulator.
//
//class MobySpace : public Moby::TimeSteppingSimulator
class MobySpace : public boost::enable_shared_from_this<MobySpace>
{
    inline boost::weak_ptr<MobySpace> weak_space() {
        //return boost::dynamic_pointer_cast<MobySpace>(shared_from_this());
        return shared_from_this();
    }

public:

    // information about the kinematics of the body
    class KinBodyInfo : public UserData
    {
public:
        class LINK : public Moby::RigidBody
        {
public:
            virtual ~LINK() {
            }

            // TODO: Transform may need to internally pass pose and pose
            // may need to be maintained for the link
            virtual void GetWorldTransform(Ravelin::MatrixNd& comWorldTrans) const
            {
                comWorldTrans = GetRavelinTransform( plink->GetTransform()*tlocal );
            }

            virtual void SetWorldTransform(const Ravelin::MatrixNd& comWorldTrans)
            {
                plink->SetTransform(GetTransform(comWorldTrans)*tlocal.inverse());
            }

            Moby::RigidBodyPtr _body;
            KinBody::LinkPtr plink;
            Transform tlocal;     /// local offset transform to account for inertias not aligned to axes
        };

        KinBodyInfo(boost::shared_ptr<Moby::Simulator> world, bool bPhysics) : _world(world), _bPhysics(bPhysics) {
            nLastStamp = 0;
        }


        virtual ~KinBodyInfo() {
            Reset();
        }

        void Reset()
        {
            FOREACH(itlink, vlinks) {
                // in Moby all bodies in the simulated world are DynamicBody
                _world->remove_dynamic_body((*itlink)->_body);
            }
        }

        KinBodyPtr pbody;     ///< body associated with this structure
        int nLastStamp;     // verify if necessary
        std::vector<boost::shared_ptr<LINK> > vlinks;     ///< if body is disabled, then geom is static (it can't be connected to a joint!)
        UserDataPtr _geometrycallback;
        boost::weak_ptr<MobySpace> _mobyspace;

private:
        boost::shared_ptr<Moby::Simulator> _world;
        bool _bPhysics;     // verify if necessary
    };

    typedef boost::shared_ptr<KinBodyInfo> KinBodyInfoPtr;
    typedef boost::shared_ptr<KinBodyInfo const> KinBodyInfoConstPtr;
    typedef boost::function<KinBodyInfoPtr(KinBodyConstPtr)> GetInfoFn;
    typedef boost::function<void (KinBodyInfoPtr)> SynchronizeCallbackFn;

    MobySpace(EnvironmentBasePtr penv, const GetInfoFn& infofn, bool bPhysics) : _penv(penv), GetInfo(infofn), _bPhysics(bPhysics) {
    }
    virtual ~MobySpace() {
    }

    //bool InitEnvironment(boost::shared_ptr<btCollisionWorld> world)
    bool InitEnvironment(boost::shared_ptr<Moby::TimeSteppingSimulator> world)
    {

        //_world = boost::shared_ptr<Moby::TimeSteppingSimulator>( new Moby::TimeSteppingSimulator() );
  
        _world = world;

        return true;
    }

    void DestroyEnvironment()
    {
        _world.reset();
    }

    //KinBodyInfoPtr InitKinBody(KinBodyPtr pbody, KinBodyInfoPtr pinfo = KinBodyInfoPtr(), btScalar fmargin=0.0005) //  -> changed fmargin because penetration was too little. For collision the values needs to be changed. There will be an XML interface for fmargin.
    KinBodyInfoPtr InitKinBody(KinBodyPtr pbody, KinBodyInfoPtr pinfo = KinBodyInfoPtr(), double fmargin=0.0005) //  -> changed fmargin because penetration was too little. For collision the values needs to be changed. There will be an XML interface for fmargin.
    {
	KinBody::KinBodyStateSaver saver(pbody);
	pbody->SetTransform(Transform());
	std::vector<dReal> vzeros(pbody->GetDOF(), 0);
	pbody->SetDOFValues(vzeros);
	
        if( !pinfo ) {
            pinfo.reset(new KinBodyInfo(_world,_bPhysics));
        }
        pinfo->Reset();
        pinfo->pbody = pbody;
        pinfo->_mobyspace = weak_space();
        pinfo->vlinks.reserve(pbody->GetLinks().size());

        //Create an articulated body
        Moby::RCArticulatedBodyPtr mobody( new Moby::RCArticulatedBody() );

        FOREACHC(itlink, pbody->GetLinks()) {
            boost::shared_ptr<KinBodyInfo::LINK> link(new KinBodyInfo::LINK());

            // create a Moby::RigidBody
            // NOTE: the mass reference frame may not be centered.  
            //   Do the Moby primitives generally assume a centered frame?
            AABB bb = (*itlink)->ComputeLocalAABB();
            Moby::PrimitivePtr prim(new Moby::BoxPrimitive(bb.extents.x*2,bb.extents.y*2,bb.extents.z*2));
            Moby::RigidBodyPtr child(new Moby::RigidBody());
            prim->set_mass((*itlink)->GetMass());
            
            child->set_visualization_data(prim->create_visualization());
            child->set_inertia(prim->get_inertia());
            child->set_enabled(true);
            // TODO: set contact parameters?
            
            // NOTE: mass is a link component and numerous geometries may
            //   be attached to the link.  Per Rosen, the link geometry
            //   can instead be approximated with a box instead rather than
            //   trying to match the whole set of geometric primitives 
            //   so the below should be reconsidered 

            /*
            FOREACHC(itgeom, (*itlink)->GetGeometries()) {
                KinBody::Link::GeometryPtr geom = *itgeom;
                Moby::RigidBodyPtr child(new Moby::RigidBody());
                Moby::PrimitivePtr prim;
                //KinBody::GeometryInfo info = 
                switch(geom->GetType()) {
                case GT_None:
                    break;
                case GT_Box: {
                    //child.reset(new btBoxShape(GetBtVector(geom->GetBoxExtents())));
                    Ravelin::Vector3d len = GetRavelinVector(geom->GetBoxExtents())*2;
                    prim = Moby::PrimitivePtr(new Moby::BoxPrimitive(len.x(),len.y(),len.z()));
                    prim->set_mass(geom->GetMass());

                    child->set_visualization_data(prim->create_visualization());
                    child->set_inertia(prim->get_inertia());
                    child->set_enabled(true);
                    // TODO: set contact parameters?
                    }
                    break;
                case GT_Sphere:
                    //child.reset(new btSphereShape(geom->GetSphereRadius()));
                    prim = Moby::PrimitivePtr(new Moby::SpherePrimitive(geom->GetSphereRadius()));
                    prim->set_mass(geom->GetMass());

                    child->set_visualization_data(prim->create_visualization());
                    child->set_inertia(prim->get_inertia());
                    child->set_enabled(true);
                    // TODO: set contact parameters?
                    break;
                case GT_Cylinder:
                    // cylinder axis aligned to Y
                    //child.reset(new btCylinderShapeZ(btVector3(geom->GetCylinderRadius(),geom->GetCylinderRadius(),geom->GetCylinderHeight()*0.5f)));
                    prim = Moby::PrimitivePtr(new Moby::CylinderPrimitive(geom->GetCylinderRadius(),geom->GetCylinderHeight()));
                    prim->set_mass(geom->GetMass());

                    child->set_visualization_data(prim->create_visualization());
                    child->set_inertia(prim->get_inertia());
                    child->set_enabled(true);
                    // TODO: set contact parameters?
                    break;
                case GT_TriMesh: {
                    prim = Moby::PrimitivePtr(new Moby::TriangleMeshPrimitive());
                    // TODO: build primitive
                    // TODO: add the primitive to the object
                    }
                    break;
                default:
                    break;
                }
            }
            */
        }

/*
        // create all ode bodies and joints
        if( !pinfo ) {
            pinfo.reset(new KinBodyInfo(_world,_bPhysics));
        }
        pinfo->Reset();
        pinfo->pbody = pbody;
        pinfo->_mobyspace = weak_space();
        pinfo->vlinks.reserve(pbody->GetLinks().size());

        FOREACHC(itlink, pbody->GetLinks()) {
            boost::shared_ptr<KinBodyInfo::LINK> link(new KinBodyInfo::LINK());

            btCompoundShape* pshapeparent = new btCompoundShape();
            link->shape.reset(pshapeparent);
            pshapeparent->setMargin(fmargin);     // need to set margin very small for collision : 0.000001

            // add all the correct geometry objects
            FOREACHC(itgeom, (*itlink)->GetGeometries()) {
                boost::shared_ptr<btCollisionShape> child;
                KinBody::Link::GeometryPtr geom = *itgeom;
                switch(geom->GetType()) {
                case GT_None:
                    break;
                case GT_Box:
                    child.reset(new btBoxShape(GetBtVector(geom->GetBoxExtents())));
                    break;
                case GT_Sphere:
                    child.reset(new btSphereShape(geom->GetSphereRadius()));
                    break;
                case GT_Cylinder:
                    // cylinder axis aligned to Y
                    child.reset(new btCylinderShapeZ(btVector3(geom->GetCylinderRadius(),geom->GetCylinderRadius(),geom->GetCylinderHeight()*0.5f)));
                    break;
                case GT_TriMesh: {
                    if( geom->GetCollisionMesh().indices.size() >= 3 ) {
                        btTriangleMesh* ptrimesh = new btTriangleMesh();

                        // for some reason adding indices makes everything crash
                        for(size_t i = 0; i < geom->GetCollisionMesh().indices.size(); i += 3) {
                            ptrimesh->addTriangle(GetBtVector(geom->GetCollisionMesh().vertices[i]), GetBtVector(geom->GetCollisionMesh().vertices[i+1]), GetBtVector(geom->GetCollisionMesh().vertices[i+2]));
                        }
                        //child.reset(new btBvhTriangleMeshShape(ptrimesh, true, true)); // doesn't do tri-tri collisions!

                        if( _bPhysics ) {
                            RAVELOG_DEBUG("converting triangle mesh to convex hull for physics\n");
                            boost::shared_ptr<btConvexShape> pconvexbuilder(new btConvexTriangleMeshShape(ptrimesh));
                            pconvexbuilder->setMargin(fmargin);

                            //Create a hull shape to approximate Trimesh
                            boost::shared_ptr<btShapeHull> hull(new btShapeHull(pconvexbuilder.get()));
                            hull->buildHull(fmargin);

                            btConvexHullShape* convexShape = new btConvexHullShape();
                            convexShape->setLocalScaling(btVector3(1,1,1));
                            //ofstream f((*itlink)->GetName().c_str());
                            for (int i=0; i< hull->numVertices(); i++) {
                                convexShape->addPoint(hull->getVertexPointer()[i]);
                                //f << hull->getVertexPointer()[i].getX() << " " << hull->getVertexPointer()[i].getY() << " " << hull->getVertexPointer()[i].getZ() << endl;
                            }
                            child.reset(convexShape);
                            delete ptrimesh;
                        }
                        else {
                            btGImpactMeshShape* pgimpact = new btGImpactMeshShape(ptrimesh);
                            pgimpact->setMargin(fmargin);     // need to set margin very small (we're not simulating anyway)
                            pgimpact->updateBound();
                            child.reset(pgimpact);
                            link->listmeshes.push_back(boost::shared_ptr<btStridingMeshInterface>(ptrimesh));
                        }
                    }
                    break;
                }
                default:
                    break;
                }

                if( !child && geom->GetType() != GT_None ) {
                    RAVELOG_WARN("did not create geom type 0x%x\n", geom->GetType());
                    continue;
                }

                link->listchildren.push_back(child);
                child->setMargin(fmargin);     // need to set margin very small (we're not simulating anyway)
                pshapeparent->addChildShape(GetBtTransform(geom->GetTransform()), child.get());
            }

            link->plink = *itlink;
            link->tlocal = (*itlink)->GetLocalMassFrame();

            if( _bPhysics ) {
                // set the mass and inertia and extract the eigenvectors of the tensor
                btVector3 localInertia = GetBtVector((*itlink)->GetPrincipalMomentsOfInertia());
		
                dReal mass = (*itlink)->GetMass();
                // -> bullet expects static objects to have zero mass                
		if((*itlink)->IsStatic()){
			mass = 0;
		}
                if( mass < 0 ) {
                    RAVELOG_WARN(str(boost::format("body %s:%s mass is %f. filling dummy values")%pbody->GetName()%(*itlink)->GetName()%mass));
                    mass = 1e-7;
                }
                else if( (*itlink)->GetPrincipalMomentsOfInertia().lengthsqr3() <= 0 ) {
                    localInertia = btVector3(1e-7,1e-7,1e-7);
                }
                btRigidBody::btRigidBodyConstructionInfo rbInfo(mass,link.get(),pshapeparent,localInertia);
                rbInfo.m_startWorldTransform = GetBtTransform((*itlink)->GetTransform()*link->tlocal);
                link->_rigidbody.reset(new btRigidBody(rbInfo));
                link->obj = link->_rigidbody;
            }
            else {
                link->obj.reset(new btCollisionObject());
                link->obj->setCollisionShape(pshapeparent);
                link->obj->setWorldTransform(GetBtTransform((*itlink)->GetTransform()*link->tlocal));
            }

            link->obj->setUserPointer(link.get());
            // Dynamic (moving) rigidbodies: positive mass, every simulation frame the dynamics will update its world transform
            // Static rigidbodies: zero mass, cannot move but just collide
            // Kinematic rigidbodies: zero mass, can be animated by the user, but there will be only one-way interaction
            link->obj->setCollisionFlags((*itlink)->IsStatic() ? btCollisionObject::CF_KINEMATIC_OBJECT : 0);
           // --> check for static
           if( _bPhysics && !(*itlink)->IsStatic() ) {
                _worlddynamics->addRigidBody(link->_rigidbody.get());
            }
            else {
                _world->addCollisionObject(link->obj.get());
            }

            //Activates all kinematic objects added to btDiscreteDynamicsWorld
            //link->body->setActivationState(DISABLE_DEACTIVATION);

            link->obj->activate(true);
            pinfo->vlinks.push_back(link);
        }

        if( _bPhysics ) {
            vector<KinBody::JointPtr> vbodyjoints; vbodyjoints.reserve(pbody->GetJoints().size()+pbody->GetPassiveJoints().size());
            vbodyjoints.insert(vbodyjoints.end(),pbody->GetJoints().begin(),pbody->GetJoints().end());
            vbodyjoints.insert(vbodyjoints.end(),pbody->GetPassiveJoints().begin(),pbody->GetPassiveJoints().end());
            FOREACH(itjoint, vbodyjoints) {
                btRigidBody* body0 = NULL, *body1 = NULL;
                if( !!(*itjoint)->GetFirstAttached() ) {
                    body0 = dynamic_cast<btRigidBody*>(pinfo->vlinks.at((*itjoint)->GetFirstAttached()->GetIndex())->obj.get());
                }
                if( !!(*itjoint)->GetSecondAttached() ) {
                    body1 = dynamic_cast<btRigidBody*>(pinfo->vlinks.at((*itjoint)->GetSecondAttached()->GetIndex())->obj.get());
                }
                if( !body0 || !body1 ) {
                    RAVELOG_ERROR(str(boost::format("joint %s needs to be attached to two bodies!\n")%(*itjoint)->GetName()));
                    continue;
                }

                Transform t0inv = GetTransform(body0->getWorldTransform()).inverse();
                Transform t1inv = GetTransform(body1->getWorldTransform()).inverse();
                boost::shared_ptr<btTypedConstraint> joint;
                switch((*itjoint)->GetType()) {
                case KinBody::JointHinge: {
                    btVector3 pivotInA = GetBtVector(t0inv * (*itjoint)->GetAnchor());
                    btVector3 pivotInB = GetBtVector(t1inv * (*itjoint)->GetAnchor());
                    btVector3 axisInA = GetBtVector(t0inv.rotate((*itjoint)->GetAxis(0)));
                    btVector3 axisInB = GetBtVector(t1inv.rotate((*itjoint)->GetAxis(0)));
                    boost::shared_ptr<btHingeConstraint> hinge(new btHingeConstraint(*body0, *body1, pivotInA, pivotInB, axisInA, axisInB));
                    //hinge->setParam(BT_CONSTRAINT_STOP_ERP,0.8);
                    //hinge->setParam(BT_CONSTRAINT_STOP_CFM,0);
                    //hinge->setParam(BT_CONSTRAINT_CFM,0);
                    vector<dReal> vupper,vlower;
                    (*itjoint)->GetLimits(vlower,vupper);
                    hinge->setLimit(vlower.at(0),vupper.at(0),0.9f,0.9f,1.0f);
                    if( !(*itjoint)->IsCircular(0) ) {
                        vector<dReal> vlower, vupper;
                        (*itjoint)->GetLimits(vlower,vupper);
                        btScalar orInitialAngle = (*itjoint)->GetValue(0);
                        btScalar btInitialAngle = hinge->getHingeAngle();
                        btScalar lower_adj, upper_adj;
                        btScalar diff = (btInitialAngle + orInitialAngle);
                        lower_adj = diff - vupper.at(0);
                        upper_adj = diff - vlower.at(0);
                        hinge->setLimit(lower_adj,upper_adj);
                    }
                    joint = hinge;
                    break;
                }
                case KinBody::JointSlider: {
                    Transform tslider; tslider.rot = quatRotateDirection(Vector(1,0,0),(*itjoint)->GetAxis(0));
                    btTransform frameInA = GetBtTransform(t0inv*tslider);
                    btTransform frameInB = GetBtTransform(t1inv*tslider);
                    joint.reset(new btSliderConstraint(*body0, *body1, frameInA, frameInB, true));
                    break;
                }
                case KinBody::JointSpherical: {
                    btVector3 pivotInA = GetBtVector(t0inv * (*itjoint)->GetAnchor());
                    btVector3 pivotInB = GetBtVector(t1inv * (*itjoint)->GetAnchor());
                    boost::shared_ptr<btPoint2PointConstraint> spherical(new btPoint2PointConstraint(*body0, *body1, pivotInA, pivotInB));
                    joint = spherical;
                    break;
                }
                case KinBody::JointUniversal:
                    RAVELOG_ERROR("universal joint not supported by bullet\n");
                    break;
                case KinBody::JointHinge2:
                    RAVELOG_ERROR("hinge2 joint not supported by bullet\n");
                    break;
                default:
                    RAVELOG_ERROR("unknown joint type 0x%8.8x\n", (*itjoint)->GetType());
                    break;
                }

                if( !!joint ) {
                    KinBody::LinkPtr plink0 = (*itjoint)->GetFirstAttached(), plink1 = (*itjoint)->GetSecondAttached();
                    int minindex = min(plink0->GetIndex(), plink1->GetIndex());
                    int maxindex = max(plink0->GetIndex(), plink1->GetIndex());

                    bool bIgnoreCollision = pbody->GetAdjacentLinks().find(minindex|(maxindex<<16)) != pbody->GetAdjacentLinks().end() || plink0->IsRigidlyAttached(plink0);
                    _worlddynamics->addConstraint(joint.get(), bIgnoreCollision);
                    pinfo->_mapjoints[*itjoint] = joint;
                }
            }
        }

        pinfo->_geometrycallback = pbody->RegisterChangeCallback(KinBody::Prop_LinkGeometry, boost::bind(&MobySpace::GeometryChangedCallback,boost::bind(&utils::sptr_from<MobySpace>, weak_space()),KinBodyWeakPtr(pbody)));
        _Synchronize(pinfo);
        return pinfo;
*/
    }

    void Synchronize()
    {
        vector<KinBodyPtr> vbodies;
        _penv->GetBodies(vbodies);
        FOREACHC(itbody, vbodies) {
            KinBodyInfoPtr pinfo = GetInfo(*itbody);
            BOOST_ASSERT( pinfo->pbody == *itbody );
            if( pinfo->nLastStamp != (*itbody)->GetUpdateStamp() ) {
                _Synchronize(pinfo);
            }
        }
    }

    void Synchronize(KinBodyConstPtr pbody)
    {
        KinBodyInfoPtr pinfo = GetInfo(pbody);
        BOOST_ASSERT( pinfo->pbody == pbody );
        if( pinfo->nLastStamp != pbody->GetUpdateStamp() ) {
            _Synchronize(pinfo);
        }
    }

    //boost::shared_ptr<btCollisionObject> GetLinkBody(KinBody::LinkConstPtr plink)
    Moby::RigidBodyPtr GetLinkBody(KinBody::LinkConstPtr plink)
    {
        KinBodyInfoPtr pinfo = GetInfo(plink->GetParent());
        BOOST_ASSERT(pinfo->pbody == plink->GetParent() );
        return pinfo->vlinks.at(plink->GetIndex());
    }

    //boost::shared_ptr<btTypedConstraint> GetJoint(KinBody::JointConstPtr pjoint)
    Moby::JointPtr GetJoint(KinBody::JointConstPtr pjoint)
    {
        KinBodyInfoPtr pinfo = GetInfo(pjoint->GetParent());
        BOOST_ASSERT(pinfo->pbody == pjoint->GetParent() );
/*
        // TODO:
        KinBodyInfo::MAPJOINTS::const_iterator it;
        it =  pinfo->_mapjoints.find(pjoint);   //  --> fixed bug
        BOOST_ASSERT(it != pinfo->_mapjoints.end());
        return it->second;
*/
    }

    void SetSynchronizationCallback(const SynchronizeCallbackFn &synccallback) {
        _synccallback = synccallback;
    }

    // NOTE: Probably should get pose due to frame references
    //static inline Transform GetTransform(const btTransform &t)
    static inline Transform GetTransform(const Ravelin::MatrixNd &t)
    {
/*
        return Transform(Vector(t.getRotation().getW(), t.getRotation().getX(), t.getRotation().getY(), t.getRotation().getZ()), Vector(t.getOrigin().getX(), t.getOrigin().getY(), t.getOrigin().getZ()));
*/
    }

    static inline Ravelin::MatrixNd GetRavelinTransform(const Transform &t)
    {
/*
        OPENRAVE_ASSERT_OP(RaveFabs(t.rot.lengthsqr4()-1),<=,0.01);
        return btTransform(btQuaternion(t.rot.y,t.rot.z,t.rot.w,t.rot.x),GetBtVector(t.trans));
*/
    }

    static inline Ravelin::Vector3d GetRavelinVector(const Vector &v)
    {
        return Ravelin::Vector3d(v.x,v.y,v.z);
    }
    bool IsInitialized() {
        return !!_world;
    }


private:

    void _Synchronize(KinBodyInfoPtr pinfo)
    {
/*
        vector<Transform> vtrans;
        std::vector<int> dofbranches;
        pinfo->pbody->GetLinkTransformations(vtrans,dofbranches);
        pinfo->nLastStamp = pinfo->pbody->GetUpdateStamp();
        BOOST_ASSERT( vtrans.size() == pinfo->vlinks.size() );
        for(size_t i = 0; i < vtrans.size(); ++i) {
            pinfo->vlinks[i]->obj->getWorldTransform() = GetBtTransform(vtrans[i]*pinfo->vlinks[i]->tlocal);
        }
        if( !!_synccallback ) {
            _synccallback(pinfo);
        }
*/
    }

/*
    virtual void GeometryChangedCallback(KinBodyWeakPtr _pbody)
    {
        EnvironmentMutex::scoped_lock lock(_penv->GetMutex());
        KinBodyPtr pbody(_pbody);
        KinBodyInfoPtr pinfo = GetInfo(pbody);
        if( !pinfo ) {
            return;
        }
        BOOST_ASSERT(boost::shared_ptr<MobySpace>(pinfo->_mobyspace) == shared_from_this());
        BOOST_ASSERT(pinfo->pbody==pbody);
        InitKinBody(pbody,pinfo);
    }

    EnvironmentBasePtr _penv;
    GetInfoFn GetInfo;
    boost::shared_ptr<btCollisionWorld> _world;
    boost::shared_ptr<btDiscreteDynamicsWorld> _worlddynamics;
    SynchronizeCallbackFn _synccallback;
    bool _bPhysics;
*/

private:
    EnvironmentBasePtr _penv;
    GetInfoFn GetInfo;
    boost::shared_ptr<Moby::TimeSteppingSimulator> _world;
    SynchronizeCallbackFn _synccallback;
    bool _bPhysics;

};
/*
static KinBody::LinkPtr GetLinkFromCollision(const btCollisionObject* co) {
    BOOST_ASSERT(co != NULL);
    return static_cast<MobySpace::KinBodyInfo::LINK*>(co->getUserPointer())->plink;
}

static KinBody::LinkPtr GetLinkFromProxy(btBroadphaseProxy* proxy) {
    return GetLinkFromCollision(static_cast<btCollisionObject*>(proxy->m_clientObject));
}
*/
/*
class OpenRAVEFilterCallback : public btOverlapFilterCallback
{
public:
    virtual bool CheckLinks(KinBody::LinkPtr plink0, KinBody::LinkPtr plink1) const = 0;
    virtual bool needBroadphaseCollision(btBroadphaseProxy* proxy0,btBroadphaseProxy* proxy1) const
    {
        BOOST_ASSERT( static_cast<btCollisionObject*>(proxy0->m_clientObject) != NULL );
        BOOST_ASSERT( static_cast<btCollisionObject*>(proxy1->m_clientObject) != NULL );
        KinBody::LinkPtr plink0 = GetLinkFromProxy(proxy0);
        KinBody::LinkPtr plink1 = GetLinkFromProxy(proxy1);
        if( !plink0->IsEnabled() || !plink1->IsEnabled() ) {
            return false;
        }
        return CheckLinks(plink0,plink1);
    }
};
*/
#ifdef RAVE_REGISTER_BOOST
#include BOOST_TYPEOF_INCREMENT_REGISTRATION_GROUP()
BOOST_TYPEOF_REGISTER_TYPE(MobySpace)
BOOST_TYPEOF_REGISTER_TYPE(MobySpace::KinBodyInfo)
BOOST_TYPEOF_REGISTER_TYPE(MobySpace::KinBodyInfo::LINK)
BOOST_TYPEOF_REGISTER_TYPE(dJointID)
#endif

#endif
