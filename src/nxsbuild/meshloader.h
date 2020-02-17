/*
Nexus

Copyright(C) 2012 - Federico Ponchio
ISTI - Italian National Research Council - Visual Computing Lab

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License (http://www.gnu.org/licenses/gpl.txt)
for more details.
*/
#ifndef NX_MESHLOADER_H
#define NX_MESHLOADER_H

#include <QString>
#include "trianglesoup.h"
#include <vcg/space/point3.h>
#include <vcg/space/box3.h>
#include "../common/material.h"

class MeshLoader {
public:
	MeshLoader(): has_colors(false), has_normals(false), has_textures(false), quantization(0) {}
	virtual ~MeshLoader() {}
	void setVertexQuantization(float q) { quantization = q; }

	virtual void setMaxMemory(quint64 max_memory) = 0;
	//returns actual number of triangles to pointers, memory must be allocated //if null, ignore.
	virtual quint32 getTriangles(quint32 size, Triangle *triangle) = 0;
	virtual quint32 getVertices(quint32 size, Splat *vertex) = 0;
	virtual bool hasColors() { return has_colors; } //call this
	virtual bool hasNormals() { return has_normals; } //call this
	virtual bool hasTextures() { return has_textures; }
	
	vcg::Point3d origin = vcg::Point3d(0, 0, 0);
	vcg::Box3d box;
	
	std::vector<BuildMaterial> materials;
	//when returning triangles add materialOffset to refer to the correct texture in stream.
	int materialOffset = 0;
	//std::vector<QString> texture_filenames;
	//int texOffset;

	
protected:
	bool has_colors;
	bool has_normals;
	bool has_textures;
	float quantization;

	virtual void quantize(float &value);
};

#endif // NX_MESHLOADER_H
