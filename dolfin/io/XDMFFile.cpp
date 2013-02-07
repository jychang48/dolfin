// Copyright (C) 2012 Chris N. Richardson and Garth N. Wells
//
// This file is part of DOLFIN.
//
// DOLFIN is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// DOLFIN is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with DOLFIN. If not, see <http://www.gnu.org/licenses/>.
//
// Modified by Garth N. Wells, 2012
//
// First added:  2012-05-28
// Last changed: 2013-02-05

#ifdef HAS_HDF5

#include <ostream>
#include <sstream>
#include <vector>
#include <boost/assign.hpp>
#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>

#include "pugixml.hpp"

#include <dolfin/function/Function.h>
#include <dolfin/function/FunctionSpace.h>
#include <dolfin/fem/GenericDofMap.h>
#include <dolfin/la/GenericVector.h>
#include <dolfin/mesh/Cell.h>
#include <dolfin/mesh/MeshEntityIterator.h>
#include <dolfin/mesh/Mesh.h>
#include <dolfin/mesh/Vertex.h>
#include "HDF5File.h"
#include "HDF5Interface.h"
#include "XDMFFile.h"

using namespace dolfin;

//----------------------------------------------------------------------------
XDMFFile::XDMFFile(const std::string filename) : GenericFile(filename, "XDMF")
{
  // Make name for HDF5 file (used to store data)
  boost::filesystem::path p(filename);
  p.replace_extension(".h5");
  hdf5_filename = p.string();

  // Create HDF5 file (truncate)
  hdf5_file.reset(new HDF5File(hdf5_filename, "w"));

  // Re-write mesh (true, false or auto, with auto based on detecting
  // changes in a hash key)
  //std::set<std::string> mesh_modes =  boost::assign::list_of("true")("false")("auto");
  //parameters.add("rewrite_mesh", "auto", mesh_modes);
  parameters.add("rewrite_function_mesh", true);

  // Flush datasets to disk at each timestep
  parameters.add("flush_output", false);
}
//----------------------------------------------------------------------------
XDMFFile::~XDMFFile()
{
  // Do nothing
}
//----------------------------------------------------------------------------
void XDMFFile::operator<< (const Function& u)
{
  std::pair<const Function*, double> ut(&u, (double) counter);
  *this << ut;
}
//----------------------------------------------------------------------------
void XDMFFile::operator<< (const std::pair<const Function*, double> ut)
{
  // FIXME: Split up this function?

  dolfin_assert(ut.first);
  const Function &u = *(ut.first);
  const double time_step = ut.second;

  // Update any ghost values
  u.update();

  // Get Mesh object
  dolfin_assert(u.function_space()->mesh());
  const Mesh& mesh = *u.function_space()->mesh();

  // Geometric dimension
  const std::size_t gdim = mesh.geometry().dim();

  // Get DOF map
  dolfin_assert(u.function_space()->dofmap());
  const GenericDofMap& dofmap = *u.function_space()->dofmap();

  // Get some Function and cell information
  const std::size_t value_rank = u.value_rank();
  const std::size_t value_size = u.value_size();
  const std::size_t cell_dim = mesh.topology().dim();
  std::size_t padded_value_size = value_size;

  // Test for cell-centred data
  std::size_t cell_based_dim = 1;
  for (std::size_t i = 0; i < value_rank; i++)
    cell_based_dim *= cell_dim;
  const bool vertex_data = !(dofmap.max_cell_dimension() == cell_based_dim);

  // Get number of local/global cells/vertices

  const std::size_t num_local_cells = mesh.num_cells();
  const std::size_t num_local_vertices = mesh.num_vertices();
  const std::size_t num_global_cells = MPI::sum(num_local_cells);
  const std::size_t num_total_vertices = MPI::sum(num_local_vertices);

  // Get Function data at vertices/cell centres
  std::vector<double> data_values;

  std::size_t num_local_entities = 0;
  std::size_t num_total_entities = 0;

  if (vertex_data)
  {
    num_local_entities = num_local_vertices; // includes duplicates
    num_total_entities = num_total_vertices;
    u.compute_vertex_values(data_values, mesh);
  }
  else
  {
    num_local_entities = num_local_cells;
    num_total_entities = num_global_cells;
    const GenericVector& v = *u.vector();
    v.get_local(data_values);
  }

  // Interleave the values for vector or tensor fields and pad 2D
  // vectors and tensors to 3D
  if (value_rank > 0)
  {
    if (value_size == 2)
      padded_value_size = 3;
    if (value_size == 4)
      padded_value_size = 9;

    std::vector<double> _data_values(padded_value_size*num_local_entities, 0.0);
    for(std::size_t i = 0; i < num_local_entities; i++)
    {
      for (std::size_t j = 0; j < value_size; j++)
      {
        std::size_t tensor_2d_offset = (j > 1 && value_size == 4) ? 1 : 0;
        _data_values[i*padded_value_size + j + tensor_2d_offset]
            = data_values[i + j*num_local_entities];
      }
    }
    data_values = _data_values;
  }

  // FIXME: Below is messy. Should query HDF5 file writer for existing
  //        mesh name
  // Write mesh to HDF5 file
  if (parameters["rewrite_function_mesh"])
  {
    current_mesh_name = "/VisualisationMesh/" + boost::lexical_cast<std::string>(counter);
    hdf5_file->write_visualisation(mesh, current_mesh_name);
  }

  // Vertex/cell values are saved in the hdf5 group /VisualisationVector
  // as distinct from /Vector which is used for solution vectors.

  // Save data values to HDF5 file

  std::vector<std::size_t> global_size(2);
  global_size[0] = num_total_entities;
  global_size[1] = padded_value_size;

  hdf5_file->write_data("/VisualisationVector/" + boost::lexical_cast<std::string>(counter),
                        data_values, global_size);

  // Flush file. Improves chances of recovering data if interrupted. Also
  // makes file somewhat readable between writes.
  if(parameters["flush_output"])
    hdf5_file->flush();

  // Write the XML meta description (see http://www.xdmf.org) on process zero
  if (MPI::process_number() == 0)
  {
    // Working data structure for formatting XML file
    std::string s;
    pugi::xml_document xml_doc;
    pugi::xml_node xdmf_timegrid;
    pugi::xml_node xdmf_timedata;

    if (counter == 0)
    {
      // First time step - create document template, adding a mesh and
      // an empty time-series
      xml_doc.append_child(pugi::node_doctype).set_value("Xdmf SYSTEM \"Xdmf.dtd\" []");
      pugi::xml_node xdmf = xml_doc.append_child("Xdmf");
      xdmf.append_attribute("Version") = "2.0";
      xdmf.append_attribute("xmlns:xi") = "http://www.w3.org/2001/XInclude";
      pugi::xml_node xdmf_domain = xdmf.append_child("Domain");

      //  /Xdmf/Domain/Grid - actually a TimeSeries, not a spatial grid
      xdmf_timegrid = xdmf_domain.append_child("Grid");
      xdmf_timegrid.append_attribute("Name") = "TimeSeries";
      xdmf_timegrid.append_attribute("GridType") = "Collection";
      xdmf_timegrid.append_attribute("CollectionType") = "Temporal";

      //  /Xdmf/Domain/Grid/Time
      pugi::xml_node xdmf_time = xdmf_timegrid.append_child("Time");
      xdmf_time.append_attribute("TimeType") = "List";
      xdmf_timedata = xdmf_time.append_child("DataItem");
      xdmf_timedata.append_attribute("Format") = "XML";
      xdmf_timedata.append_attribute("Dimensions") = "1";
      xdmf_timedata.append_child(pugi::node_pcdata);
    }
    else
    {
      // Subsequent timestep - read in existing XDMF file
      pugi::xml_parse_result result = xml_doc.load_file(filename.c_str());
      if (!result)
      {
        dolfin_error("XDMFFile.cpp",
                     "write data to XDMF file",
                     "XML parsing error when reading from existing file");
      }

      // Get data node
      xdmf_timegrid = xml_doc.child("Xdmf").child("Domain").child("Grid");
      dolfin_assert(xdmf_timegrid);

      // Get time series node
      xdmf_timedata = xdmf_timegrid.child("Time").child("DataItem");
      dolfin_assert(xdmf_timedata);
    }

    //  Add a time step to the TimeSeries List
    xdmf_timedata.attribute("Dimensions").set_value(static_cast<unsigned int>(counter + 1));
    s = boost::lexical_cast<std::string>(xdmf_timedata.first_child().value())
          + " " + boost::str((boost::format("%d") % time_step));
    xdmf_timedata.first_child().set_value(s.c_str());

    //   /Xdmf/Domain/Grid/Grid - the actual data for this timestep
    pugi::xml_node xdmf_grid = xdmf_timegrid.append_child("Grid");
    s = u.name() + "_" + boost::lexical_cast<std::string>(counter);
    xdmf_grid.append_attribute("Name") = s.c_str();
    xdmf_grid.append_attribute("GridType") = "Uniform";

    // Grid/Topology
    pugi::xml_node xdmf_topology = xdmf_grid.append_child("Topology");
    xml_mesh_topology(xdmf_topology, cell_dim, num_global_cells,
                      current_mesh_name + "/topology");

    // Grid/Geometry
    pugi::xml_node xdmf_geometry = xdmf_grid.append_child("Geometry");
    xml_mesh_geometry(xdmf_geometry, num_total_vertices, gdim,
                      current_mesh_name + "/coordinates");

    // Grid/Attribute (Function value data)
    pugi::xml_node xdmf_values = xdmf_grid.append_child("Attribute");
    xdmf_values.append_attribute("Name") = u.name().c_str();

    if (value_rank == 0)
      xdmf_values.append_attribute("AttributeType") = "Scalar";
    else if (value_rank == 1)
      xdmf_values.append_attribute("AttributeType") = "Vector";
    else if (value_rank == 2)
      xdmf_values.append_attribute("AttributeType") = "Tensor";

    if (vertex_data)
      xdmf_values.append_attribute("Center") = "Node";
    else
      xdmf_values.append_attribute("Center") = "Cell";

    pugi::xml_node xdmf_data = xdmf_values.append_child("DataItem");
    xdmf_data.append_attribute("Format") = "HDF";

    s = boost::lexical_cast<std::string>(num_total_entities) + " "
      + boost::lexical_cast<std::string>(padded_value_size);

    xdmf_data.append_attribute("Dimensions") = s.c_str();

    boost::filesystem::path p(hdf5_filename);
    s = p.filename().string() + ":/VisualisationVector/"
          + boost::lexical_cast<std::string>(counter);
    xdmf_data.append_child(pugi::node_pcdata).set_value(s.c_str());

    // Write XML file
    xml_doc.save_file(filename.c_str(), "  ");
  }

  // Increment counter
  counter++;
}
//----------------------------------------------------------------------------
void XDMFFile::operator<< (const Mesh& mesh)
{
  // Write Mesh to HDF5 file (use contiguous vertex indices for topology)
  dolfin_assert(hdf5_file);

  warning("Mesh saved to XDMF is only suitable for visualisation.");

  // Output data name
  const std::string name = "/VisualisationMesh/" + boost::lexical_cast<std::string>(counter);
  hdf5_file->write_visualisation(mesh, name);

  // Get number of local/global cells/vertices

  const std::size_t num_local_cells = mesh.num_cells();
  const std::size_t num_global_cells = MPI::sum(num_local_cells);
  const std::size_t num_local_vertices = mesh.num_vertices();
  const std::size_t num_total_vertices = MPI::sum(num_local_vertices);
  const std::size_t cell_dim = mesh.topology().dim();

  // Get geometric dimension
  const std::size_t gdim = mesh.geometry().dim();

  // FIXME: Names should be returned by HDF5::write_mesh
  // Mesh data set names
  const std::string mesh_topology_name = name + "/topology";
  const std::string mesh_coords_name = name + "/coordinates";

  // Write the XML meta description on process zero
  if (MPI::process_number() == 0)
  {
    // Create XML document
    pugi::xml_document xml_doc;

    // XML headers
    xml_doc.append_child(pugi::node_doctype).set_value("Xdmf SYSTEM \"Xdmf.dtd\" []");
    pugi::xml_node xdmf = xml_doc.append_child("Xdmf");
    xdmf.append_attribute("Version") = "2.0";
    xdmf.append_attribute("xmlns:xi") = "http://www.w3.org/2001/XInclude";
    pugi::xml_node xdmf_domain = xdmf.append_child("Domain");
    pugi::xml_node xdmf_grid = xdmf_domain.append_child("Grid");
    xdmf_grid.append_attribute("Name") = "dolfin_mesh";
    xdmf_grid.append_attribute("GridType") = "Uniform";

    // Describe topological connectivity
    pugi::xml_node xdmf_topology = xdmf_grid.append_child("Topology");
    xml_mesh_topology(xdmf_topology, cell_dim, num_global_cells,
                      mesh_topology_name);

    // Describe geometric coordinates
    pugi::xml_node xdmf_geometry = xdmf_grid.append_child("Geometry");
    xml_mesh_geometry(xdmf_geometry, num_total_vertices, gdim,
                      mesh_coords_name);

    xml_doc.save_file(filename.c_str(), "  ");
  }
}
//----------------------------------------------------------------------------
void XDMFFile::operator<< (const MeshFunction<bool>& meshfunction)
{
  const Mesh& mesh = meshfunction.mesh();
  const std::size_t cell_dim = meshfunction.dim();

  // HDF5 does not support a boolean type, so copy to a std::size_t with values
  // 1 and 0
  MeshFunction<std::size_t> uint_meshfunction(mesh, cell_dim);
  for (MeshEntityIterator cell(mesh, cell_dim); !cell.end(); ++cell)
    uint_meshfunction[cell->index()] = (meshfunction[cell->index()] ? 1 : 0);

  write_mesh_function(uint_meshfunction);
}
//----------------------------------------------------------------------------
void XDMFFile::operator<< (const MeshFunction<int>& meshfunction)
{
  write_mesh_function(meshfunction);
}
//----------------------------------------------------------------------------
void XDMFFile::operator<< (const MeshFunction<unsigned int>& meshfunction)
{
  write_mesh_function(meshfunction);
}
//----------------------------------------------------------------------------
void XDMFFile::operator<< (const MeshFunction<unsigned long int>& meshfunction)
{
  write_mesh_function(meshfunction);
}
//----------------------------------------------------------------------------
void XDMFFile::operator<< (const MeshFunction<double>& meshfunction)
{
  write_mesh_function(meshfunction);
}
//----------------------------------------------------------------------------
template<typename T>
void XDMFFile::write_mesh_function(const MeshFunction<T>& meshfunction)
{
  // Get mesh
  const Mesh& mesh = meshfunction.mesh();

  // Get some dimensions
  const std::size_t gdim = mesh.geometry().dim();
  const std::size_t cell_dim = meshfunction.dim();

  dolfin_assert(cell_dim <= mesh.topology().dim());

  if (meshfunction.size() == 0)
  {
    dolfin_error("XDMFFile.cpp",
                 "save empty MeshFunction",
                 "No values in MeshFunction");
  }

  // Collate data in a vector
  std::vector<T> data_values(meshfunction.values(),
                             meshfunction.values() + meshfunction.size());

  dolfin_assert(hdf5_file);

  // Get counts of mesh cells and vertices
  const std::size_t num_local_cells = mesh.num_entities(cell_dim);
  const std::size_t num_local_vertices = mesh.num_vertices();
  const std::size_t num_global_cells = MPI::sum(num_local_cells);
  const std::size_t num_total_vertices = MPI::sum(num_local_vertices);

  // Work out HDF5 dataset names
  const std::string name = "/VisualisationMesh/" + boost::lexical_cast<std::string>(counter);
  const std::string mesh_topology_name = name + "/topology";
  const std::string mesh_coords_name = name + "/coordinates";

  boost::filesystem::path p(hdf5_filename);
  std::string dataset_basic_name = "/VisualisationMesh/MeshFunction_"
    + meshfunction.name() + "_" + boost::lexical_cast<std::string>(counter);
  const std::string mesh_function_dataset_name
    = p.filename().string() + ":" + dataset_basic_name;

  // Write mesh to HDF5
  hdf5_file->write_visualisation(mesh, cell_dim, name);

  // Write values to HDF5
  const std::vector<std::size_t> global_size(1, MPI::sum(data_values.size()));
  hdf5_file->write_data(dataset_basic_name, data_values, global_size);

  // Write the XML meta description (see http://www.xdmf.org) on process zero
  if (MPI::process_number() == 0)
  {
    // Create XML document
    std::string s;
    pugi::xml_document xml_doc;
    pugi::xml_node xdmf_timegrid;
    pugi::xml_node xdmf_timedata;

    if (counter == 0)
    {
      // First time step - create document template, adding a mesh and
      // an empty time-series
      xml_doc.append_child(pugi::node_doctype).set_value("Xdmf SYSTEM \"Xdmf.dtd\" []");
      pugi::xml_node xdmf = xml_doc.append_child("Xdmf");
      xdmf.append_attribute("Version") = "2.0";
      xdmf.append_attribute("xmlns:xi") = "http://www.w3.org/2001/XInclude";
      pugi::xml_node xdmf_domain = xdmf.append_child("Domain");

      //  /Xdmf/Domain/Grid - actually a TimeSeries, not a spatial grid
      xdmf_timegrid = xdmf_domain.append_child("Grid");
      xdmf_timegrid.append_attribute("Name") = "TimeSeries";
      xdmf_timegrid.append_attribute("GridType") = "Collection";
      xdmf_timegrid.append_attribute("CollectionType") = "Temporal";

      //  /Xdmf/Domain/Grid/Time
      pugi::xml_node xdmf_time = xdmf_timegrid.append_child("Time");
      xdmf_time.append_attribute("TimeType") = "List";
      xdmf_timedata = xdmf_time.append_child("DataItem");
      xdmf_timedata.append_attribute("Format") = "XML";
      xdmf_timedata.append_attribute("Dimensions") = "1";
      xdmf_timedata.append_child(pugi::node_pcdata);
    }
    else
    {
      // Subsequent timestep - read in existing XDMF file
      pugi::xml_parse_result result = xml_doc.load_file(filename.c_str());
      if (!result)
      {
        dolfin_error("XDMFFile.cpp",
                     "write data to XDMF file",
                     "XML parsing error when reading from existing file");
      }

      // Get data node
      xdmf_timegrid = xml_doc.child("Xdmf").child("Domain").child("Grid");
      dolfin_assert(xdmf_timegrid);

      // Get time series node
      xdmf_timedata = xdmf_timegrid.child("Time").child("DataItem");
      dolfin_assert(xdmf_timedata);
    }

    //  Add a time step to the TimeSeries List
    xdmf_timedata.attribute("Dimensions").set_value(static_cast<unsigned int>(counter + 1));
    s = boost::lexical_cast<std::string>(xdmf_timedata.first_child().value())
          + " " + boost::str((boost::format("%d") % counter));
    xdmf_timedata.first_child().set_value(s.c_str());

    //   /Xdmf/Domain/Grid/Grid - the actual data for this timestep
    pugi::xml_node xdmf_grid = xdmf_timegrid.append_child("Grid");
    s = "meshfunction_" + boost::lexical_cast<std::string>(counter);
    xdmf_grid.append_attribute("Name") = s.c_str();
    xdmf_grid.append_attribute("GridType") = "Uniform";

    // Topological connectivity
    pugi::xml_node xdmf_topology = xdmf_grid.append_child("Topology");
    xml_mesh_topology(xdmf_topology, cell_dim, num_global_cells,
                      mesh_topology_name);

    // Geometric coordinate positions
    pugi::xml_node xdmf_geometry = xdmf_grid.append_child("Geometry");
    xml_mesh_geometry(xdmf_geometry, num_total_vertices, gdim,
                      mesh_coords_name);

    // Make reference to MeshFunction value data and dimensions
    pugi::xml_node xdmf_vals = xdmf_grid.append_child("Attribute");
    xdmf_vals.append_attribute("Name") = meshfunction.name().c_str();
    xdmf_vals.append_attribute("AttributeType") = "Scalar";
    xdmf_vals.append_attribute("Center") = "Cell";

    pugi::xml_node xdmf_data = xdmf_vals.append_child("DataItem");
    xdmf_data.append_attribute("Format") = "HDF";
    std::string data_dims = boost::lexical_cast<std::string>(num_global_cells) + " 1";
    xdmf_data.append_attribute("Dimensions") = data_dims.c_str();

    xdmf_data.append_child(pugi::node_pcdata).set_value(mesh_function_dataset_name.c_str());

    // Output to storage
    xml_doc.save_file(filename.c_str(), "  ");
  }

  counter++;
}
//----------------------------------------------------------------------------
void XDMFFile::xml_mesh_topology(pugi::xml_node &xdmf_topology,
                                 const std::size_t cell_dim,
                                 const std::size_t num_global_cells,
                                 const std::string topology_dataset_name) const
{
  xdmf_topology.append_attribute("NumberOfElements") = (unsigned int) num_global_cells;

  // Cell type
  if (cell_dim == 0)
  {
    xdmf_topology.append_attribute("TopologyType") = "PolyVertex";
    xdmf_topology.append_attribute("NodesPerElement") = "1";
  }
  else if (cell_dim == 1)
  {
    xdmf_topology.append_attribute("TopologyType") = "PolyLine";
    xdmf_topology.append_attribute("NodesPerElement") = "2";
  }
  else if (cell_dim == 2)
    xdmf_topology.append_attribute("TopologyType") = "Triangle";
  else if (cell_dim == 3)
    xdmf_topology.append_attribute("TopologyType") = "Tetrahedron";

  // Refer to all cells and dimensions
  pugi::xml_node xdmf_topology_data = xdmf_topology.append_child("DataItem");
  xdmf_topology_data.append_attribute("Format") = "HDF";
  const std::string cell_dims = boost::lexical_cast<std::string>(num_global_cells)
    + " " + boost::lexical_cast<std::string>(cell_dim + 1);
  xdmf_topology_data.append_attribute("Dimensions") = cell_dims.c_str();

  // For XDMF file need to remove path from filename so that xdmf
  // filenames such as "results/data.xdmf" correctly index h5 files in
  // the same directory
  boost::filesystem::path p(hdf5_filename);
  std::string topology_reference = p.filename().string() + ":" + topology_dataset_name;
  xdmf_topology_data.append_child(pugi::node_pcdata).set_value(topology_reference.c_str());
}
//----------------------------------------------------------------------------
void XDMFFile::xml_mesh_geometry(pugi::xml_node& xdmf_geometry,
                                 const std::size_t num_total_vertices,
                                 const std::size_t gdim,
                                 const std::string geometry_dataset_name) const
{
  dolfin_assert(0 < gdim && gdim <= 3);
  std::string geometry_type;
  if (gdim == 1)
    geometry_type = "X"; // geometry "X" not supported?
  else if (gdim == 2)
    geometry_type = "XY";
  else if (gdim == 3)
    geometry_type = "XYZ";

  xdmf_geometry.append_attribute("GeometryType") = geometry_type.c_str();
  pugi::xml_node xdmf_geom_data = xdmf_geometry.append_child("DataItem");

  xdmf_geom_data.append_attribute("Format") = "HDF";
  std::string geom_dim = boost::lexical_cast<std::string>(num_total_vertices)
    + " " + boost::lexical_cast<std::string>(gdim);
  xdmf_geom_data.append_attribute("Dimensions") = geom_dim.c_str();

  boost::filesystem::path p(hdf5_filename);
  const std::string geometry_reference
    = p.filename().string() + ":" + geometry_dataset_name;
  xdmf_geom_data.append_child(pugi::node_pcdata).set_value(geometry_reference.c_str());
}
//----------------------------------------------------------------------------

#endif
