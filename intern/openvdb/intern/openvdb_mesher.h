/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2019 Martin Felke.
 * All rights reserved.
 */

#ifndef __OPENVDB_MESHER_H__
#define __OPENVDB_MESHER_H__

#include <openvdb/openvdb.h>
#include <openvdb/tools/VolumeToMesh.h>
#include "openvdb_level_set.h"

namespace openvdb {
OPENVDB_USE_VERSION_NAMESPACE
namespace OPENVDB_VERSION_NAME {
namespace tools {

template<typename GridType>
inline typename std::enable_if<std::is_scalar<typename GridType::ValueType>::value, void>::type
doVolumeToMesh(const GridType &grid,
               std::vector<Vec3s> &points,
               std::vector<Vec3I> &triangles,
               std::vector<Vec4I> &quads,
               double isovalue,
               double adaptivity,
               bool relaxDisorientedTriangles,
               BoolTreeType::Ptr adaptivityMask,
               FloatGrid::Ptr refGrid)
{

  VolumeToMesh mesher(isovalue, adaptivity, relaxDisorientedTriangles);

  mesher.setAdaptivityMask(adaptivityMask);
  mesher.setRefGrid(refGrid, adaptivity);
  mesher(grid);

  // Preallocate the point list
  points.clear();
  points.resize(mesher.pointListSize());

  {  // Copy points
    volume_to_mesh_internal::PointListCopy ptnCpy(mesher.pointList(), points);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, points.size()), ptnCpy);
    mesher.pointList().reset(nullptr);
  }

  PolygonPoolList &polygonPoolList = mesher.polygonPoolList();

  {  // Preallocate primitive lists
    size_t numQuads = 0, numTriangles = 0;
    for (size_t n = 0, N = mesher.polygonPoolListSize(); n < N; ++n) {
      openvdb::tools::PolygonPool &polygons = polygonPoolList[n];
      numTriangles += polygons.numTriangles();
      numQuads += polygons.numQuads();
    }

    triangles.clear();
    triangles.resize(numTriangles);
    quads.clear();
    quads.resize(numQuads);
  }

  // Copy primitives
  size_t qIdx = 0, tIdx = 0;
  for (size_t n = 0, N = mesher.polygonPoolListSize(); n < N; ++n) {
    openvdb::tools::PolygonPool &polygons = polygonPoolList[n];

    for (size_t i = 0, I = polygons.numQuads(); i < I; ++i) {
      quads[qIdx++] = polygons.quad(i);
    }

    for (size_t i = 0, I = polygons.numTriangles(); i < I; ++i) {
      triangles[tIdx++] = polygons.triangle(i);
    }
  }
}

/// @internal This overload is enabled only for grids that do not have a scalar ValueType.
template<typename GridType>
inline typename std::enable_if<!std::is_scalar<typename GridType::ValueType>::value, void>::type
doVolumeToMesh(const GridType &,
               std::vector<Vec3s> &,
               std::vector<Vec3I> &,
               std::vector<Vec4I> &,
               double,
               double,
               bool,
               BoolTreeType::Ptr)
{
  OPENVDB_THROW(TypeError, "volume to mesh conversion is supported only for scalar grids");
}

/// @endcond
//}

template<typename GridType>
inline void volumeToMesh(const GridType &grid,
                         std::vector<Vec3s> &points,
                         std::vector<Vec3I> &triangles,
                         std::vector<Vec4I> &quads,
                         double isovalue,
                         double adaptivity,
                         bool relaxDisorientedTriangles,
                         BoolTreeType::Ptr adaptivityMask,
                         FloatGrid::Ptr refGrid)
{
  doVolumeToMesh(grid,
                 points,
                 triangles,
                 quads,
                 isovalue,
                 adaptivity,
                 relaxDisorientedTriangles,
                 adaptivityMask,
                 refGrid);
}

// ray bbox helper code, since houdini function aint useable here
// taken from scratchapixel.com
class Ray {
 public:
  Ray(const Vec3f &orig, const Vec3f &dir) : orig(orig), dir(dir), invdir(1 / dir)
  {
    // invdir = 1 / dir;
    sign[0] = (invdir.x() < 0);
    sign[1] = (invdir.y() < 0);
    sign[2] = (invdir.z() < 0);
  }
  Vec3f orig, dir;  // ray orig and dir
  Vec3f &invdir;
  int sign[3];
};

class AABBox {
 public:
  AABBox(const Vec3f &b0, const Vec3f &b1)
  {
    bounds[0] = b0, bounds[1] = b1;
  }
  bool intersect(const Ray &r, float &t) const
  {
    float tmin, tmax, tymin, tymax, tzmin, tzmax;

    tmin = (bounds[r.sign[0]].x() - r.orig.x()) * r.invdir.x();
    tmax = (bounds[1 - r.sign[0]].x() - r.orig.x()) * r.invdir.x();
    tymin = (bounds[r.sign[1]].y() - r.orig.y()) * r.invdir.y();
    tymax = (bounds[1 - r.sign[1]].y() - r.orig.y()) * r.invdir.y();

    if ((tmin > tymax) || (tymin > tmax))
      return false;

    if (tymin > tmin)
      tmin = tymin;
    if (tymax < tmax)
      tmax = tymax;

    tzmin = (bounds[r.sign[2]].z() - r.orig.z()) * r.invdir.z();
    tzmax = (bounds[1 - r.sign[2]].z() - r.orig.z()) * r.invdir.z();

    if ((tmin > tzmax) || (tzmin > tmax))
      return false;

    if (tzmin > tmin)
      tmin = tzmin;
    if (tzmax < tmax)
      tmax = tzmax;

    t = tmin;

    if (t < 0) {
      t = tmax;
      if (t < 0)
        return false;
    }

    return true;
  }
  Vec3f bounds[2];
};

/// TBB body object for threaded sharp feature construction
template<typename IndexTreeType, typename BoolTreeType> class GenAdaptivityMaskOp {
 public:
  using BoolLeafManager = openvdb::tree::LeafManager<BoolTreeType>;

  GenAdaptivityMaskOp(OpenVDBLevelSet &lvl,
                      const IndexTreeType &indexTree,
                      BoolLeafManager &,
                      float edgetolerance = 0.0);

  void run(bool threaded = true);

  void operator()(const tbb::blocked_range<size_t> &) const;

  openvdb::Vec3s face_normal(uint32_t tri) const;

 private:
  OpenVDBLevelSet &mLvl;
  const IndexTreeType &mIndexTree;
  BoolLeafManager &mLeafs;
  float mEdgeTolerance;
};

template<typename IndexTreeType, typename BoolTreeType>
GenAdaptivityMaskOp<IndexTreeType, BoolTreeType>::GenAdaptivityMaskOp(
    OpenVDBLevelSet &lvl,
    const IndexTreeType &indexTree,
    BoolLeafManager &leafMgr,
    float edgetolerance)
    : mLvl(lvl), mIndexTree(indexTree), mLeafs(leafMgr), mEdgeTolerance(edgetolerance)
{
  mEdgeTolerance = std::max(0.0f, mEdgeTolerance);
  mEdgeTolerance = std::min(1.0f, mEdgeTolerance);
}

template<typename IndexTreeType, typename BoolTreeType>
void GenAdaptivityMaskOp<IndexTreeType, BoolTreeType>::run(bool threaded)
{
  if (threaded) {
    tbb::parallel_for(mLeafs.getRange(), *this);
  }
  else {
    (*this)(mLeafs.getRange());
  }
}

template<typename IndexTreeType, typename BoolTreeType>
void GenAdaptivityMaskOp<IndexTreeType, BoolTreeType>::operator()(
    const tbb::blocked_range<size_t> &range) const
{
  using IndexAccessorType = typename openvdb::tree::ValueAccessor<const IndexTreeType>;
  IndexAccessorType idxAcc(mIndexTree);

  // UT_Vector3 tmpN, normal;
  // GA_Offset primOffset;
  int tmpIdx;

  openvdb::Coord ijk, nijk;
  typename BoolTreeType::LeafNodeType::ValueOnIter iter;
  std::vector<uint32_t> vert_tri = mLvl.get_vert_tri();

  for (size_t n = range.begin(); n < range.end(); ++n) {
    iter = mLeafs.leaf(n).beginValueOn();
    for (; iter; ++iter) {
      ijk = iter.getCoord();

      bool edgeVoxel = false;

      int idx = idxAcc.getValue(ijk);
      uint32_t primOffset = vert_tri[idx];
      // calculate face normal...
      // normal = mRefGeo.getGEOPrimitive(primOffset)->computeNormal();
      openvdb::Vec3s normal = mLvl.face_normal(primOffset);

      for (size_t i = 0; i < 18; ++i) {
        nijk = ijk + openvdb::util::COORD_OFFSETS[i];
        if (idxAcc.probeValue(nijk, tmpIdx) && tmpIdx != idx) {
          primOffset = vert_tri[tmpIdx];
          openvdb::Vec3s tmpN = mLvl.face_normal(primOffset);

          if (normal.dot(tmpN) < mEdgeTolerance) {
            edgeVoxel = true;
            break;
          }
        }
      }

      if (!edgeVoxel)
        iter.setValueOff();
    }
  }
}

////////////////////////////////////////

////////////////////////////////////////
using RangeT = tbb::blocked_range<size_t>;

/// TBB body object for threaded world to voxel space transformation and copy of points
class TransformOp {
 public:
  TransformOp(OpenVDBLevelSet &lvl,
              const openvdb::math::Transform &transform,
              std::vector<openvdb::Vec3s> &pointList);

  void operator()(const RangeT &) const;

 private:
  OpenVDBLevelSet &mLvl;
  const openvdb::math::Transform &mTransform;
  std::vector<openvdb::Vec3s> *const mPointList;
};

////////////////////////////////////////

TransformOp::TransformOp(OpenVDBLevelSet &lvl,
                         const openvdb::math::Transform &transform,
                         std::vector<openvdb::Vec3s> &pointList)
    : mLvl(lvl), mTransform(transform), mPointList(&pointList)
{
}

void TransformOp::operator()(const RangeT &r) const
{
  openvdb::Vec3s pos;
  openvdb::Vec3d ipos;
  int i;

  for (i = r.begin(); i < r.end(); i++) {
    pos = mLvl.get_points()[i];
    ipos = mTransform.worldToIndex(openvdb::Vec3d(pos.x(), pos.y(), pos.z()));
    (*mPointList)[i] = openvdb::Vec3s(ipos);
  }
}

/// @brief   TBB body object for threaded primitive copy
/// @details Produces a primitive-vertex index list.
class PrimCpyOp {
 public:
  PrimCpyOp(OpenVDBLevelSet &mLvl, std::vector<openvdb::Vec4I> &primList);
  void operator()(const RangeT &) const;

 private:
  OpenVDBLevelSet &mLvl;
  std::vector<openvdb::Vec4I> *const mPrimList;
};

////////////////////////////////////////

PrimCpyOp::PrimCpyOp(OpenVDBLevelSet &lvl, std::vector<openvdb::Vec4I> &primList)
    : mLvl(lvl), mPrimList(&primList)
{
}

void PrimCpyOp::operator()(const RangeT &r) const
{
  openvdb::Vec4I prim;
  int i;
  for (i = r.begin(); i < r.end(); i++) {
    for (int vtx = 0; vtx < 3; ++vtx) {
      prim[vtx] = mLvl.get_triangles()[i][vtx];
    }
    prim[3] = openvdb::util::INVALID_IDX;

    (*mPrimList)[i] = prim;
  }
}

#if 0
/// @brief   TBB body object for threaded vertex normal generation
/// @details Averages face normals from all similarly oriented primitives,
///          that share the same vertex-point, to maintain sharp features.
class VertexNormalOp {
 public:
  VertexNormalOp(OpenVDBLevelSet &mLvl, float angle = 0.7f);
  void operator()(const RangeT &);

 private:
  OpenVDBLevelSet &mLvl;
  std::vector<openvdb::Vec3s> mNormalHandle;
  const float mAngle;
};

////////////////////////////////////////

VertexNormalOp::VertexNormalOp(OpenVDBLevelSet &lvl, float angle) : mLvl(lvl), mAngle(angle)
{
  mNormalHandle = std::vector<openvdb::Vec3s>(lvl.get_triangles().size());
}

void VertexNormalOp::operator()(const RangeT &r)
{
  std::vector<openvdb::Vec3I> tris = mLvl.get_triangles();
  std::vector<openvdb::Vec3s> verts = mLvl.get_points();
  std::vector<openvdb::Vec3s> vnorms = mLvl.get_vert_normals();
  openvdb::Vec3s tmpN, avgN;
  openvdb::Vec3I tri;
  int i;

  for (i = r.begin(); i < r.end(); i++) {
    tri = tris[i];
    avgN = vnorms[tri[0]];
    for (int vtx = 0; vtx < 2; vtx++) {
      tmpN = vnorms[tri[vtx]];
      if (tmpN.dot(avgN) > mAngle)
        avgN += tmpN;
    }
    avgN.normalize();
    mNormalHandle[i] = avgN;
  }
}
#endif

using RangeT = tbb::blocked_range<size_t>;
/// TBB body object for threaded sharp feature construction
class SharpenFeaturesOp {
 public:
  using EdgeData = openvdb::tools::MeshToVoxelEdgeData;

  SharpenFeaturesOp(OpenVDBLevelSet &refGeo,
                    EdgeData &edgeData,
                    const openvdb::math::Transform &xform,
                    const openvdb::BoolTree *mask = nullptr);

  void operator()(const RangeT &) const;

 private:
  OpenVDBLevelSet &mRefGeo;
  EdgeData &mEdgeData;
  const openvdb::math::Transform &mXForm;
  const openvdb::BoolTree *mMaskTree;
};

////////////////////////////////////////

SharpenFeaturesOp::SharpenFeaturesOp(OpenVDBLevelSet &refGeo,
                                     EdgeData &edgeData,
                                     const openvdb::math::Transform &xform,
                                     const openvdb::BoolTree *mask)
    : mRefGeo(refGeo), mEdgeData(edgeData), mXForm(xform), mMaskTree(mask)
{
}

void SharpenFeaturesOp::operator()(const RangeT &r) const
{
  int i;
  openvdb::tools::MeshToVoxelEdgeData::Accessor acc = mEdgeData.getAccessor();
  std::vector<openvdb::Vec3s> result(mRefGeo.get_out_points());

  using BoolAccessor = openvdb::tree::ValueAccessor<const openvdb::BoolTree>;
  std::unique_ptr<BoolAccessor> maskAcc;

  if (mMaskTree) {
    maskAcc.reset(new BoolAccessor(*mMaskTree));
  }

  openvdb::Vec3s avgP;
  openvdb::BBoxd cell;

  openvdb::Vec3d pos, normal;
  openvdb::Coord ijk;

  std::vector<openvdb::Vec3d> points(12), normals(12);
  std::vector<openvdb::Index32> primitives(12);

  for (i = r.begin(); i < r.end(); i++) {

    pos = result[i];
    pos = mXForm.worldToIndex(pos);

    ijk[0] = int(std::floor(pos[0]));
    ijk[1] = int(std::floor(pos[1]));
    ijk[2] = int(std::floor(pos[2]));

    if (maskAcc && !maskAcc->isValueOn(ijk))
      continue;

    points.clear();
    normals.clear();
    primitives.clear();

    // get voxel-edge intersections
    mEdgeData.getEdgeData(acc, ijk, points, primitives);

    avgP = openvdb::Vec3s(0.0, 0.0, 0.0);

    // get normal list
    for (size_t n = 0, N = points.size(); n < N; ++n) {

      avgP += points[n];
      normal = mRefGeo.face_normal(n);
      normals.push_back(normal);
    }

    // Calculate feature point position
    if (points.size() > 1) {

      pos = openvdb::tools::findFeaturePoint(points, normals);

      // Constrain points to stay inside their initial
      // coordinate cell.
      cell = openvdb::BBoxd(
          openvdb::Vec3d(double(ijk[0]), double(ijk[1]), double(ijk[2])),
          openvdb::Vec3d(double(ijk[0] + 1), double(ijk[1] + 1), double(ijk[2] + 1)));

      cell.expand(openvdb::Vec3d(0.3, 0.3, 0.3));

      if (!cell.isInside(openvdb::Vec3d(pos[0], pos[1], pos[2]))) {

        openvdb::Vec3s org(pos[0], pos[1], pos[2]);

        avgP *= 1.f / float(points.size());
        openvdb::Vec3s dir = avgP - org;
        dir.normalize();

        // double distance;
        // if (cell.intersectRay(org, dir, 1E17F, &distance) > 0)
        float distance;
        Ray ray(org, dir);
        AABBox box(cell.min(), cell.max());
        if (box.intersect(ray, distance)) {
          pos = org + dir * distance;
        }
      }

      pos = mXForm.indexToWorld(pos);
      result[i] = pos;
    }
  }
  mRefGeo.set_out_points(result);
}
}  // namespace tools
}  // namespace OPENVDB_VERSION_NAME
}  // namespace openvdb

#endif /* __OPENVDB_MESHER_H__ */
