/* Copyright 2020 Stanford University, NVIDIA Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __LEGION_DOMAIN_H__
#define __LEGION_DOMAIN_H__

#include "realm.h"
#include "legion/legion_types.h"

/**
 * \file legion_domain.h
 * This file provides some untyped representations of points
 * and domains as well as backwards compatibility types 
 * necessary for maintaining older versions of the runtime
 */

namespace Legion {

  template<int DIM, typename T = coord_t>
  using Point = Realm::Point<DIM,T>;
  template<int DIM, typename T = coord_t>
  using Rect = Realm::Rect<DIM,T>;
  template<int M, int N, typename T = coord_t>
  using Transform = Realm::Matrix<M,N,T>;

  /**
   * \class AffineTransform
   * An affine transform is used to transform points in one 
   * coordinate space into points in another coordinate space
   * using the basic Ax + b transformation, where A is a 
   * transform matrix and b is an offset vector
   */
  template<int M, int N, typename T = coord_t>
  struct AffineTransform {
  public:
    __CUDA_HD__
    AffineTransform(void); // default to identity transform
    // allow type coercions where possible
    template<typename T2> __CUDA_HD__
    AffineTransform(const AffineTransform<M,N,T2> &rhs);
    template<typename T2, typename T3> __CUDA_HD__
    AffineTransform(const Transform<M,N,T2> transform, 
                    const Point<M,T3> offset);
  public:
    template<typename T2> __CUDA_HD__
    AffineTransform<M,N,T>& operator=(const AffineTransform<M,N,T2> &rhs);
  public:
    // Apply the transformation to a point
    template<typename T2> __CUDA_HD__
    Point<M,T> operator[](const Point<N,T2> point) const;
    // Compose the transform with another transform
    template<int P> __CUDA_HD__
    AffineTransform<M,P,T> operator()(const AffineTransform<N,P,T> &rhs) const;
    // Test whether this is the identity transform
    __CUDA_HD__
    bool is_identity(void) const;
  public:
    // Transform = Ax + b
    Transform<M,N,T> transform; // A
    Point<M,T>       offset; // b
  };

  /**
   * \class ScaleTransform
   * A scale transform is a used to do a projection transform
   * that converts a point in one coordinate space into a range
   * in another coordinate system using the transform:
   *    [y0, y1] = Ax + [b, c]
   *              ------------
   *                   d
   *  where all lower case letters are points and A is
   *  transform matrix. Note that by making b == c then
   *  we can make this a one-to-one point mapping.
   */
  template<int M, int N, typename T = coord_t>
  struct ScaleTransform {
  public:
    __CUDA_HD__
    ScaleTransform(void); // default to identity transform
    // allow type coercions where possible
    template<typename T2> __CUDA_HD__
    ScaleTransform(const ScaleTransform<M,N,T2> &rhs);
    template<typename T2, typename T3, typename T4> __CUDA_HD__
    ScaleTransform(const Transform<M,N,T2> transform,
                   const Rect<M,T3> extent,
                   const Point<M,T4> divisor);
  public:
    template<typename T2> __CUDA_HD__
    ScaleTransform<M,N,T>& operator=(const ScaleTransform<M,N,T2> &rhs);
  public:
    // Apply the transformation to a point
    template<typename T2> __CUDA_HD__
    Rect<M,T> operator[](const Point<N,T2> point) const;
    // Test whether this is the identity transform
    __CUDA_HD__
    bool is_identity(void) const;
  public:
    Transform<M,N,T> transform; // A
    Rect<M,T>        extent; // [b=lo, c=hi]
    Point<M,T>       divisor; // d
  };

  // If we've got c++11 we can just include this directly
  template<int DIM, typename T = coord_t>
  using DomainT = Realm::IndexSpace<DIM,T>;

  /**
   * \class DomainPoint
   * This is a type erased point where the number of 
   * dimensions is a runtime value
   */
  class DomainPoint {
  public:
    enum { MAX_POINT_DIM = LEGION_MAX_DIM };

    __CUDA_HD__
    DomainPoint(void);
    __CUDA_HD__
    DomainPoint(coord_t index);
    __CUDA_HD__
    DomainPoint(const DomainPoint &rhs);
    template<int DIM, typename T> __CUDA_HD__
    DomainPoint(const Point<DIM,T> &rhs);

    template<unsigned DIM>
    operator LegionRuntime::Arrays::Point<DIM>(void) const;
    template<int DIM, typename T> __CUDA_HD__
    operator Point<DIM,T>(void) const;

    __CUDA_HD__
    DomainPoint& operator=(const DomainPoint &rhs);
    __CUDA_HD__
    bool operator==(const DomainPoint &rhs) const;
    __CUDA_HD__
    bool operator!=(const DomainPoint &rhs) const;
    __CUDA_HD__
    bool operator<(const DomainPoint &rhs) const;

    __CUDA_HD__
    coord_t& operator[](unsigned index);
    __CUDA_HD__
    const coord_t& operator[](unsigned index) const;

    struct STLComparator {
      __CUDA_HD__
      bool operator()(const DomainPoint& a, const DomainPoint& b) const
      {
        if(a.dim < b.dim) return true;
        if(a.dim > b.dim) return false;
        for(int i = 0; (i == 0) || (i < a.dim); i++) {
          if(a.point_data[i] < b.point_data[i]) return true;
          if(a.point_data[i] > b.point_data[i]) return false;
        }
        return false;
      }
    };

    template<int DIM>
    static DomainPoint from_point(
        typename LegionRuntime::Arrays::Point<DIM> p);

    __CUDA_HD__
    Color get_color(void) const;
    __CUDA_HD__
    coord_t get_index(void) const;
    __CUDA_HD__
    int get_dim(void) const;

    template <int DIM>
    LegionRuntime::Arrays::Point<DIM> get_point(void) const; 

    __CUDA_HD__
    bool is_null(void) const;

    __CUDA_HD__
    static DomainPoint nil(void);

  protected:
  public:
    int dim;
    coord_t point_data[MAX_POINT_DIM];

    friend std::ostream& operator<<(std::ostream& os, const DomainPoint& dp);
  };

  /**
   * \class Domain
   * This is a type erased rectangle where the number of 
   * dimensions is stored as a runtime value
   */
  class Domain {
  public:
    typedef ::realm_id_t IDType;
    // Keep this in sync with legion_domain_max_rect_dim_t
    // in legion_config.h
    enum { MAX_RECT_DIM = LEGION_MAX_DIM };
    __CUDA_HD__
    Domain(void);
    __CUDA_HD__
    Domain(const Domain& other);
    __CUDA_HD__
    Domain(const DomainPoint &lo, const DomainPoint &hi);

    template<int DIM, typename T> __CUDA_HD__
    Domain(const Rect<DIM,T> &other);

    template<int DIM, typename T> __CUDA_HD__
    Domain(const DomainT<DIM,T> &other);

    __CUDA_HD__
    Domain& operator=(const Domain& other);

    __CUDA_HD__
    bool operator==(const Domain &rhs) const;
    __CUDA_HD__
    bool operator!=(const Domain &rhs) const;
    __CUDA_HD__
    bool operator<(const Domain &rhs) const;

    static const Domain NO_DOMAIN;

    __CUDA_HD__
    bool exists(void) const;
    __CUDA_HD__
    bool dense(void) const;

    template<int DIM, typename T> __CUDA_HD__
    Rect<DIM,T> bounds(void) const;

    template<int DIM>
    static Domain from_rect(typename LegionRuntime::Arrays::Rect<DIM> r);

    template<int DIM>
    static Domain from_point(typename LegionRuntime::Arrays::Point<DIM> p);

    template<int DIM>
    operator LegionRuntime::Arrays::Rect<DIM>(void) const;

    template<int DIM, typename T> __CUDA_HD__
    operator Rect<DIM,T>(void) const;

    template<int DIM, typename T>
    operator DomainT<DIM,T>(void) const;

    // Only works for structured DomainPoint.
    static Domain from_domain_point(const DomainPoint &p);

    // No longer supported
    //Realm::IndexSpace get_index_space(void) const;

    __CUDA_HD__
    bool is_valid(void) const;

    bool contains(DomainPoint point) const;

    // This will only check the bounds and not the sparsity map
    __CUDA_HD__
    bool contains_bounds_only(DomainPoint point) const;

    __CUDA_HD__
    int get_dim(void) const;

    bool empty(void) const;

    size_t get_volume(void) const;

    __CUDA_HD__
    DomainPoint lo(void) const;

    __CUDA_HD__
    DomainPoint hi(void) const;

    // Intersects this Domain with another Domain and returns the result.
    Domain intersection(const Domain &other) const;

    // Returns the bounding box for this Domain and a point.
    // WARNING: only works with structured Domain.
    Domain convex_hull(const DomainPoint &p) const;

    template <int DIM>
    LegionRuntime::Arrays::Rect<DIM> get_rect(void) const; 

    class DomainPointIterator {
    public:
      DomainPointIterator(const Domain& d);
      DomainPointIterator(const DomainPointIterator &rhs);

      bool step(void);

      operator bool(void) const;
      DomainPoint& operator*(void);
      DomainPointIterator& operator=(const DomainPointIterator &rhs);
      DomainPointIterator& operator++(void);
      DomainPointIterator operator++(int /*i am postfix*/);
    public:
      DomainPoint p;
      // Some buffers that we will do in-place new statements to in
      // order to not have to call new/delete in our implementation
      char is_iterator[
              sizeof(Realm::IndexSpaceIterator<MAX_RECT_DIM,coord_t>)];
      char rect_iterator[
              sizeof(Realm::PointInRectIterator<MAX_RECT_DIM,coord_t>)];
      bool is_valid, rect_valid;
    };
  protected:
  public:
    IDType is_id;
#ifdef __CUDA_ARCH__
    // Work around an internal nvcc bug by marking this volatile 
    volatile
#endif
    int dim;
    coord_t rect_data[2 * MAX_RECT_DIM];
  };

  template<int DIM, typename COORD_T = coord_t>
  class PointInRectIterator {
  public:
    __CUDA_HD__
    PointInRectIterator(void);
    __CUDA_HD__
    PointInRectIterator(const Rect<DIM,COORD_T> &r,
                        bool column_major_order = true);
  public:
    __CUDA_HD__
    inline bool valid(void) const;
    __CUDA_HD__
    inline bool step(void);
  public:
    __CUDA_HD__
    inline bool operator()(void) const;
    __CUDA_HD__
    inline Point<DIM,COORD_T> operator*(void) const;
    __CUDA_HD__
    inline COORD_T operator[](unsigned index) const;
    __CUDA_HD__
    inline const Point<DIM,COORD_T>* operator->(void) const;
    __CUDA_HD__
    inline PointInRectIterator<DIM,COORD_T>& operator++(void);
    __CUDA_HD__
    inline PointInRectIterator<DIM,COORD_T> operator++(int/*postfix*/);
  protected:
    Realm::PointInRectIterator<DIM,COORD_T> itr;
  };

  template<int DIM, typename COORD_T = coord_t>
  class RectInDomainIterator {
  public:
    RectInDomainIterator(void);
    RectInDomainIterator(const DomainT<DIM,COORD_T> &d);
  public:
    inline bool valid(void) const;
    inline bool step(void);
  public:
    inline bool operator()(void) const;
    inline Rect<DIM,COORD_T> operator*(void) const;
    inline const Rect<DIM,COORD_T>* operator->(void) const;
    inline RectInDomainIterator<DIM,COORD_T>& operator++(void);
    inline RectInDomainIterator<DIM,COORD_T> operator++(int/*postfix*/);
  protected:
    Realm::IndexSpaceIterator<DIM,COORD_T> itr;
  };

  template<int DIM, typename COORD_T = coord_t>
  class PointInDomainIterator {
  public:
    PointInDomainIterator(void);
    PointInDomainIterator(const DomainT<DIM,COORD_T> &d,
                          bool column_major_order = true);
  public:
    inline bool valid(void) const;
    inline bool step(void); 
  public:
    inline bool operator()(void) const;
    inline Point<DIM,COORD_T> operator*(void) const;
    inline COORD_T operator[](unsigned index) const; 
    inline const Point<DIM,COORD_T>* operator->(void) const;
    inline PointInDomainIterator& operator++(void);
    inline PointInDomainIterator operator++(int /*postfix*/);
  protected:
    RectInDomainIterator<DIM,COORD_T> rect_itr;
    PointInRectIterator<DIM,COORD_T> point_itr;
    bool column_major;
  };

  /**
   * \class DomainTransform
   * A type-erased version of a Transform for removing template
   * parameters from a Transform object
   */
  class DomainTransform {
  public:
    __CUDA_HD__
    DomainTransform(void);
    __CUDA_HD__
    DomainTransform(const DomainTransform &rhs);
    template<int M, int N, typename T> __CUDA_HD__
    DomainTransform(const Transform<M,N,T> &rhs);
  public:
    __CUDA_HD__
    DomainTransform& operator=(const DomainTransform &rhs);
    template<int M, int N, typename T> __CUDA_HD__
    DomainTransform& operator=(const Transform<M,N,T> &rhs);
  public:
    template<int M, int N, typename T> __CUDA_HD__
    operator Transform<M,N,T>(void) const;
  public:
    __CUDA_HD__
    DomainPoint operator*(const DomainPoint &p) const;
  public:
    __CUDA_HD__
    bool is_identity(void) const;
  public:
    int m, n;
    coord_t matrix[LEGION_MAX_DIM * LEGION_MAX_DIM];
  };

  /**
   * \class DomainAffineTransform
   * A type-erased version of an AffineTransform for removing
   * template parameters from an AffineTransform type
   */
  class DomainAffineTransform {
  public:
    __CUDA_HD__
    DomainAffineTransform(void);
    __CUDA_HD__
    DomainAffineTransform(const DomainAffineTransform &rhs);
    __CUDA_HD__
    DomainAffineTransform(const DomainTransform &t, const DomainPoint &p);
    template<int M, int N, typename T> __CUDA_HD__
    DomainAffineTransform(const AffineTransform<M,N,T> &transform);
  public:
    __CUDA_HD__
    DomainAffineTransform& operator=(const DomainAffineTransform &rhs);
    template<int M, int N, typename T> __CUDA_HD__
    DomainAffineTransform& operator=(const AffineTransform<M,N,T> &rhs);
  public:
    template<int M, int N, typename T> __CUDA_HD__
    operator AffineTransform<M,N,T>(void) const;
  public:
    // Apply the transformation to a point
    __CUDA_HD__
    DomainPoint operator[](const DomainPoint &p) const;
    // Test for the identity
    __CUDA_HD__
    bool is_identity(void) const;
  public:
    DomainTransform transform;
    DomainPoint     offset;
  };

  /**
   * \class DomainScaleTransform
   * A type-erased version of a ScaleTransform for removing
   * template parameters from a ScaleTransform type
   */
  class DomainScaleTransform {
  public:
    __CUDA_HD__
    DomainScaleTransform(void);
    __CUDA_HD__
    DomainScaleTransform(const DomainScaleTransform &rhs);
    __CUDA_HD__
    DomainScaleTransform(const DomainTransform &transform,
                         const Domain &extent, const DomainPoint &divisor);
    template<int M, int N, typename T> __CUDA_HD__
    DomainScaleTransform(const ScaleTransform<M,N,T> &transform);
  public:
    __CUDA_HD__
    DomainScaleTransform& operator=(const DomainScaleTransform &rhs);
    template<int M, int N, typename T> __CUDA_HD__
    DomainScaleTransform& operator=(const ScaleTransform<M,N,T> &rhs);
  public:
    template<int M, int N, typename T> __CUDA_HD__
    operator ScaleTransform<M,N,T>(void) const;
  public:
    // Apply the transformation to a point
    __CUDA_HD__
    Domain operator[](const DomainPoint &p) const;
    // Test for the identity
    __CUDA_HD__
    bool is_identity(void) const;
  public:
    DomainTransform transform;
    Domain          extent;
    DomainPoint     divisor;
  };

  /**
   * \class Span
   * A span class is used for handing back allocations of elements with
   * a uniform stride that users can safely access simply by indexing
   * the pointer as an array of elements. Note that the Legion definition
   * of a span does not guarantee that elements are contiguous the same
   * as the c++20 definition of a span.
   */
  template<typename FT, PrivilegeMode PM = LEGION_READ_WRITE>
  class Span {
  public:
    class iterator : 
      public std::iterator<std::random_access_iterator_tag,FT> {
    public:
      iterator(void) : ptr(NULL), stride(0) { } 
    private:
      iterator(uint8_t *p, size_t s) : ptr(p), stride(s) { }
    public:
      inline iterator& operator=(const iterator &rhs) 
        { ptr = rhs.ptr; stride = rhs.stride; return *this; }
      inline iterator& operator+=(int rhs) { ptr += stride; return *this; }
      inline iterator& operator-=(int rhs) { ptr -= stride; return *this; }
      inline FT& operator*(void) const { return *reinterpret_cast<FT*>(ptr); }
      inline FT* operator->(void) const { return reinterpret_cast<FT*>(ptr); }
      inline FT& operator[](int rhs) const
        { return *reinterpret_cast<FT*>(ptr + rhs * stride); }
    public:
      inline iterator& operator++(void) { ptr += stride; return *this; }
      inline iterator& operator--(void) { ptr -= stride; return *this; }
      inline iterator operator++(int) 
        { iterator it(ptr, stride); ptr += stride; return it; }
      inline iterator operator--(int) 
        { iterator it(ptr, stride); ptr -= stride; return it; }
      inline iterator operator+(int rhs) const 
        { return iterator(ptr + stride * rhs, stride); }
      inline iterator operator-(int rhs) const 
        { return iterator(ptr - stride * rhs, stride); }
    public:
      inline bool operator==(const iterator &rhs) const 
        { return (ptr == rhs.ptr); }
      inline bool operator!=(const iterator &rhs) const 
        { return (ptr != rhs.ptr); }
      inline bool operator<(const iterator &rhs) const 
        { return (ptr < rhs.ptr); }
      inline bool operator>(const iterator &rhs) const 
        { return (ptr > rhs.ptr); }
      inline bool operator<=(const iterator &rhs) const 
        { return (ptr <= rhs.ptr); }
      inline bool operator>=(const iterator &rhs) const
        { return (ptr >= rhs.ptr); }
    private:
      uint8_t *ptr;
      size_t stride;
    };
    class reverse_iterator : 
      public std::iterator<std::random_access_iterator_tag,FT> {
    public:
      reverse_iterator(void) : ptr(NULL), stride(0) { } 
    private:
      reverse_iterator(uint8_t *p, size_t s) : ptr(p), stride(s) { }
    public:
      inline reverse_iterator& operator=(const reverse_iterator &rhs) 
        { ptr = rhs.ptr; stride = rhs.stride; return *this; }
      inline reverse_iterator& operator+=(int rhs) 
        { ptr -= stride; return *this; }
      inline reverse_iterator& operator-=(int rhs) 
        { ptr += stride; return *this; }
      inline FT& operator*(void) const { return *reinterpret_cast<FT*>(ptr); }
      inline FT* operator->(void) const { return reinterpret_cast<FT*>(ptr); }
      inline FT& operator[](int rhs) const
        { return *reinterpret_cast<FT*>(ptr - rhs * stride); }
    public:
      inline reverse_iterator& operator++(void) 
        { ptr -= stride; return *this; }
      inline reverse_iterator& operator--(void) 
        { ptr += stride; return *this; }
      inline reverse_iterator operator++(int) 
        { reverse_iterator it(ptr, stride); ptr -= stride; return it; }
      inline reverse_iterator operator--(int) 
        { reverse_iterator it(ptr, stride); ptr += stride; return it; }
      inline reverse_iterator operator+(int rhs) const 
        { return reverse_iterator(ptr - stride * rhs, stride); }
      inline reverse_iterator operator-(int rhs) const 
        { return reverse_iterator(ptr + stride * rhs, stride); }
    public:
      inline bool operator==(const reverse_iterator &rhs) const
        { return (ptr == rhs.ptr); }
      inline bool operator!=(const reverse_iterator &rhs) const
        { return (ptr != rhs.ptr); }
      inline bool operator<(const reverse_iterator &rhs) const
        { return (ptr > rhs.ptr); }
      inline bool operator>(const reverse_iterator &rhs) const
        { return (ptr < rhs.ptr); }
      inline bool operator<=(const reverse_iterator &rhs) const
        { return (ptr >= rhs.ptr); }
      inline bool operator>=(const reverse_iterator &rhs) const
        { return (ptr <= rhs.ptr); }
    private:
      uint8_t *ptr;
      size_t stride;
    };
  public:
    Span(void) : base(NULL), extent(0), stride(0) { }
    Span(FT *b, size_t e, size_t s = sizeof(FT))
      : base(reinterpret_cast<uint8_t*>(b)), extent(e), stride(s) { }
  public:
    inline iterator begin(void) const { return iterator(base, stride); }
    inline iterator end(void) const 
      { return iterator(base + extent*stride, stride); }
    inline reverse_iterator rbegin(void) const
      { return reverse_iterator(base + (extent-1) * stride, stride); }
    inline reverse_iterator rend(void) const
      { return reverse_iterator(base - stride, stride); }
  public:
    inline FT& front(void) const { return *reinterpret_cast<FT*>(base); }
    inline FT& back(void) const
      { return *reinterpret_cast<FT*>(base + (extent-1)*stride); }
    inline FT& operator[](int index) const
      { return *reinterpret_cast<FT*>(base + index * stride); }
    inline FT* data(void) const { return reinterpret_cast<FT*>(base); }
    inline uintptr_t get_base(void) const { return uintptr_t(base); }
  public:
    inline size_t size(void) const { return extent; }
    inline size_t step(void) const { return stride; }
    inline bool empty(void) const { return (extent == 0); }
  private:
    uint8_t *base;
    size_t extent; // number of elements
    size_t stride; // byte stride
  };

}; // namespace Legion

#include "legion/legion_domain.inl"

#endif // __LEGION_DOMAIN_H__

