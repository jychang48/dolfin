// Copyright (C) 2002 Johan Hoffman and Anders Logg.
// Licensed under the GNU GPL Version 2.

#include <dolfin/dolfin_log.h>
#include <dolfin/PDE.h>
#include <dolfin/FiniteElement.h>

using namespace dolfin;

//-----------------------------------------------------------------------------
PDE::PDE(int dim)
{
  h  = 0.0;
  t  = 0.0;
  k = 0.0;
  
  this->dim = dim;
  
  noeq = 1; // Will be set otherwise by EquationSystem
}
//-----------------------------------------------------------------------------
PDE::~PDE()
{
  
}
//-----------------------------------------------------------------------------
void PDE::updateLHS(const FiniteElement* element,
			 const Cell*          cell,
			 const Map*       map,
			 const Quadrature*    quadrature)
{
  // Common update for LHS and RHS
  update(element, cell, map, quadrature);
  
  // Local update of LHS
  updateLHS();
}
//-----------------------------------------------------------------------------
void PDE::updateRHS(const FiniteElement* element,
			 const Cell*          cell,
			 const Map*       map,
			 const Quadrature*    quadrature)
{
  // Common update for LHS and RHS
  update(element, cell, map, quadrature);

  // Local update of RHS
  updateRHS();
}
//-----------------------------------------------------------------------------
real PDE::dx(real a) const
{
  return 0.0;
}
//-----------------------------------------------------------------------------
real PDE::dy(real a) const
{
  return 0.0;
}
//-----------------------------------------------------------------------------
real PDE::dz(real a) const
{
  return 0.0;
}
//-----------------------------------------------------------------------------
real PDE::dt(real a) const
{
  return 0.0;
}
//-----------------------------------------------------------------------------
const ElementFunction& PDE::dx(const ShapeFunction &v) const
{
  return v.dx();
}
//-----------------------------------------------------------------------------
const ElementFunction& PDE::dy(const ShapeFunction &v) const
{
  return v.dy();
}
//-----------------------------------------------------------------------------
const ElementFunction& PDE::dz(const ShapeFunction &v) const
{
  return v.dz();
}
//-----------------------------------------------------------------------------
const ElementFunction& PDE::dt(const ShapeFunction &v) const
{
  return v.dt();
}
//-----------------------------------------------------------------------------
const ElementFunction PDE::dx(const Product &v) const
{
  return map->dx(v);
}
//-----------------------------------------------------------------------------
const ElementFunction PDE::dy(const Product &v) const
{
  return map->dy(v);
}
//-----------------------------------------------------------------------------
const ElementFunction PDE::dz(const Product &v) const
{
  return map->dz(v);
}
//-----------------------------------------------------------------------------
const ElementFunction PDE::dt(const Product &v) const
{
  return map->dt(v);
}
//-----------------------------------------------------------------------------
const ElementFunction PDE::dx(const ElementFunction &v) const
{
  return map->dx(v);
}
//-----------------------------------------------------------------------------
const ElementFunction PDE::dy(const ElementFunction &v) const
{
  return map->dy(v);
}
//-----------------------------------------------------------------------------
const ElementFunction PDE::dz(const ElementFunction &v) const
{
  return map->dz(v);
}
//-----------------------------------------------------------------------------
const ElementFunction PDE::dt(const ElementFunction &v) const
{
  return map->dt(v);
}
//-----------------------------------------------------------------------------
const FunctionSpace::ElementFunction::Vector
PDE::grad(const ShapeFunction &v)
{
  FunctionSpace::ElementFunction::Vector w(v.dx(), v.dy(), v.dz());
  return w;
}
//-----------------------------------------------------------------------------
void PDE::add(ElementFunction& v, Function& f)
{
  FunctionPair p(v, f);
  functions.add(p);
}
//-----------------------------------------------------------------------------
void PDE::add(ElementFunction::Vector& v, Function::Vector& f)
{
  add(v(0), f(0));
  add(v(1), f(1));
  add(v(2), f(2));
}
//-----------------------------------------------------------------------------
void PDE::update(const FiniteElement* element,
		 const Cell*          cell,
		 const Map*       map,
		 const Quadrature*    quadrature)
{
  // Update element functions
  for (List<FunctionPair>::Iterator p(functions); !p.end(); ++p)
    p->update(*element, *cell, t);
  
  // Update integral measures
  dK.update(*map, *quadrature);
  dS.update(*map, *quadrature);

  // Save map (to compute derivatives)
  this->map = map;
}
//-----------------------------------------------------------------------------
// PDE::FunctionPair
//-----------------------------------------------------------------------------
PDE::FunctionPair::FunctionPair()
{
  v = 0;
  f = 0;
}
//-----------------------------------------------------------------------------
PDE::FunctionPair::FunctionPair(ElementFunction &v, Function &f)
{
  this->v = &v;
  this->f = &f;
}
//-----------------------------------------------------------------------------
void PDE::FunctionPair::update
(const FiniteElement &element, const Cell &cell, real t)
{  
  f->update(*v, element, cell, t);
}
//-----------------------------------------------------------------------------
