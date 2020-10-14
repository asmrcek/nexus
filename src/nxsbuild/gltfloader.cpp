#include "gltfloader.h"
#include "../common/gltf.h"

#include <draco/compression/decode.h>
#include <QFile>

#include <iostream>
#include <set>

using namespace std;
using namespace fx;


//PROBLEM int8_t for b.color_map: if we have a lot of textures this would be a problem.
//we could use a temporary array if int342_t with enum (seee materials)
//TODO might want to use gltf materials directly!
int8_t GltfLoader::addTexture(BuildMaterial &m, int8_t &i, std::map<int32_t, int8_t> &remap) {
	if(i < 0) return i;
	assert(i < doc->textures.size());
	gltf::Texture &texture = doc->textures[i];
	gltf::Image &image = doc->images[texture.source];

	if(image.uri.size() == 0 || image.IsEmbeddedResource()) {
		if(image.IsEmbeddedResource()) {
			throw "Embedded resources still not supported for images";
		}
		//just extract the image
		gltf::BufferView &view = doc->bufferViews[image.bufferView];
		gltf::Buffer &buffer = doc->buffers[view.buffer];
		QString ext;
		if(image.mimeType == "image/png")
			ext = "png";
		else if (image.mimeType == "image/jpeg")
			ext = "jpg";
		else
			throw QString("Unsupported image mimetype: ") + image.mimeType.c_str();
		QString filename = QString("cache_img%1.").arg(image.bufferView) + ext;
		image.uri = filename.toStdString();
		QFile file(filename);
		file.open(QFile::WriteOnly);
		file.write((char *)buffer.data.data() + view.byteOffset, view.byteLength);
	}
	i = remap[i];
	m.textures[i] = QString(image.uri.c_str());
	return i;
}

BuildMaterial GltfLoader::convertMaterial(gltf::Material &m) {
	BuildMaterial b;
	for(int i = 0; i < 4; i++)
		b.color[i] =  m.pbrMetallicRoughness.baseColorFactor[i];
	b.metallic = m.pbrMetallicRoughness.metallicFactor;
	b.roughness = m.pbrMetallicRoughness.roughnessFactor;

	b.norm_scale = m.normalTexture.scale;
	//TODO specular, glossiness. read from extension
	b.occlusion = m.occlusionTexture.strength;


	b.color_map = m.pbrMetallicRoughness.baseColorTexture.index;
	b.metallic_map = m.pbrMetallicRoughness.metallicRoughnessTexture.index;
	b.normal_map = m.normalTexture.index;
	//TODO bump, specular and glossiness!
	b.occlusion_map = m.occlusionTexture.index;

	auto remap = b.countMaps();
	//we need to remap to 0 -> nmaps-1
	b.textures.resize(b.nmaps);
	addTexture(b, b.color_map, remap);
	addTexture(b, b.metallic_map, remap);
	addTexture(b, b.normal_map, remap);
	addTexture(b, b.occlusion_map, remap);
	//TODO specular, bum and glossiness

	if(b.nmaps)
		has_textures = true;

	b.flipY = false;
	return b;
}

GltfLoader::GltfLoader(QString filename): vertices("cache_gltfvertex") {

	QFile file(filename);
	if(!file.exists())
		throw QString("Could not find file: ") + filename;

	//NO QUOTAS!
	gltf::ReadQuotas readQuotas{};
	readQuotas.MaxBufferCount = 1<<20;
	readQuotas.MaxBufferByteLength = 1<<30;
	readQuotas.MaxFileSize = 1<<30;

	try {
		doc = new gltf::Document;
		if(filename.endsWith("glb")) {
			*doc = fx::gltf::LoadFromBinary(filename.toStdString(), readQuotas);
		} else {
			*doc = fx::gltf::LoadFromText(filename.toStdString(), readQuotas);
		}

		gltf::Document &model = *doc;

		//look for buffers where to read.
		//having more than one mesh and one primitives should be supported.
		//we need to index them to materials

		//model.meshes.resize(1);
		if(model.meshes.size() > 1)
			cout << "TODO: more than 1 mesh! I just hope the attributes are the same, for the moment." << endl;
					//throw QString("At the moment only 1 mesh per gltf is supported");

		gltf::Mesh &mesh = model.meshes[0];
		if(mesh.primitives.size() == 0)
			throw QString("No primitives in the mesh.");

		//make sure all primitives have the same attributes (it's not really needed.
		gltf::Primitive &primitive = model.meshes[0].primitives[0];
		for(auto &mesh: model.meshes) {
			for(auto &p: mesh.primitives) {
				if(p.mode != gltf::Primitive::Mode::Triangles)
					throw QString("Unimplemented primitive mode, only triangles for now");

				if(p.indices < 0)
					throw QString("Point cloud not supported at the moment for gltf");

				for(auto a: primitive.attributes)
					if(!p.attributes.count(a.first))
						throw QString("Primitives in the mesh have different attributes");
				for(auto a: p.attributes)
					if(!primitive.attributes.count(a.first))
						throw QString("Primitives in the mesh have different attributes");
			}
		}

		set<string> supported = { "TEXCOORD_0", "NORMAL", "POSITION", "COLOR_0" };
		//we only support TEXCOORD_0, NORMAL, POSITION, COLOR_0
		for(auto &a: primitive.attributes) {
			if(!supported.count(a.first))
				cout << "Attribute " << a.first << " not supported yet" << endl;
			//throw QString("Attribute ") + QString(a.first.c_str()) + " not supported (yet)";
		}

		has_colors = primitive.attributes.count("COLOR_0");
		has_normals = false; //we should preserve normals at least at lowest level! primitive.attributes.count("NORMAL");
		has_textures = primitive.attributes.count("TEXCOORD_0");

		cout << "TODO: If the -u is specified for gltf, adjust material\n";


		current_mesh = 0;
		current_primitive = 0;

		for(auto &material: model.materials)
			materials.push_back(convertMaterial(material));


		//draco support

		for(auto &mesh: model.meshes) {
			for(auto &primitive: mesh.primitives) {
				if(primitive.extensionsAndExtras.is_null())
					continue;


				nlohmann::json &extensions = primitive.extensionsAndExtras["extensions"];
				cout << extensions << endl;
				if(!extensions.count("KHR_draco_mesh_compression"))
					continue;

				cout << "Draco compressed" << endl;
				auto &dra = extensions["KHR_draco_mesh_compression"];
				int bufferViewId = dra["bufferView"];

				gltf::BufferView &view = doc->bufferViews[bufferViewId];
				gltf::Buffer &gltf_buffer = doc->buffers[view.buffer];

				const char *ptr = (char *)gltf_buffer.data.data() + view.byteOffset;
				uint32_t length = view.byteLength;

				draco::DecoderBuffer buffer;
				buffer.Init(ptr, length);

				draco::EncodedGeometryType geom_type = draco::Decoder::GetEncodedGeometryType(&buffer).value();
				draco::Decoder decoder;
				if (geom_type != draco::TRIANGULAR_MESH)
					throw "Unsupported point clouds!";
				unique_ptr<draco::Mesh> dmesh = decoder.DecodeMeshFromBuffer(&buffer).value();
				int32_t nf = dmesh->num_faces();

				gltf::Buffer face_buffer;
				face_buffer.byteLength = nf * 3 * 12;
				face_buffer.data.resize(face_buffer.byteLength);

				gltf::BufferView face_view;
				face_view.buffer = doc->buffers.size();
				face_view.byteLength = face_buffer.byteLength;
				face_view.target = gltf::BufferView::TargetType::ElementArrayBuffer;
				face_view.byteOffset = 0;
				face_view.byteStride = 12;
				doc->bufferViews.push_back(face_view);



				gltf::Accessor &face_accessor = doc->accessors[primitive.indices];
				face_accessor.bufferView = doc->bufferViews.size()-1;
				face_accessor.byteOffset = 0;
				face_accessor.componentType = gltf::Accessor::ComponentType::UnsignedInt;
				assert(face_accessor.count == nf*3);

				int max = 0;
				uint32_t *face_ptr = (uint32_t *)face_buffer.data.data();
				for(uint32_t i = 0; i < nf; i++) {
					draco::FaceIndex f(i);
					for(int k = 0 ;k < 3; k++) {
						int n = face_ptr[i*3 + k] = dmesh->face(f)[k].value();
						max = std::max(n, max);
					}
				}
				doc->buffers.push_back(face_buffer); //only after having it filled.

				int nvert = dmesh->num_points();

				auto &dra_attributes = dra["attributes"];

				for(auto dra_attr: dra_attributes.items()) {
					const draco::PointAttribute *attr = dmesh->GetAttributeByUniqueId(dra_attr.value());

					std::string attribute_name = dra_attr.key(); //COLOR POSITION ETC -> this needs to be matched to the primitives attributes
					if(!primitive.attributes.count(attribute_name))
						throw "Mismatch between draco attributes and primitive attribtues";

					gltf::Accessor &v_accessor = doc->accessors[primitive.attributes[attribute_name]];
					int num_components = 1;
					switch(v_accessor.type) {
					case gltf::Accessor::Type::Scalar: num_components = 1;break;
					case gltf::Accessor::Type::Vec2: num_components = 2; break;
					case gltf::Accessor::Type::Vec3: num_components = 3; break;
					case gltf::Accessor::Type::Vec4: num_components = 4; break;
					default: throw "Unsupported vectors above 4. Sorry."; break;
					}
					int component_size = 4;
					switch(v_accessor.componentType) {
					case gltf::Accessor::ComponentType::Float:
					case gltf::Accessor::ComponentType::UnsignedInt: component_size = 4; break;
					case gltf::Accessor::ComponentType::Short:
					case gltf::Accessor::ComponentType::UnsignedShort: component_size = 2; break;
					case gltf::Accessor::ComponentType::Byte:
					case gltf::Accessor::ComponentType::UnsignedByte: component_size = 1; break;
					default: break;
					}

					if(num_components != attr->num_components())
						throw "Mismatch in draco glft attributes";

					gltf::Buffer v_buffer;
					v_buffer.byteLength = nvert * num_components * component_size;
					v_buffer.data.resize(v_buffer.byteLength);
					for(int i = 0; i < nvert; i++) {
						draco::AttributeValueIndex mapped = attr->mapped_index(draco::PointIndex(i));
						attr->GetValue(mapped, v_buffer.data.data() + i*num_components*component_size);
					}
					doc->buffers.push_back(v_buffer);

					gltf::BufferView v_view;
					v_view.buffer = doc->buffers.size() -1;
					v_view.byteLength = v_buffer.byteLength;
					v_view.target = gltf::BufferView::TargetType::ElementArrayBuffer;
					v_view.byteOffset = 0;
					v_view.byteStride = num_components*component_size;
					doc->bufferViews.push_back(v_view);

					v_accessor.bufferView = doc->bufferViews.size()-1;
				}
			}
		}


	} catch(QString s) {
		throw s;
	} catch (std::runtime_error error) {
		cout << error.what() << endl;
	} catch(const char *s) {
		throw QString(s);
	} catch(...) {
		throw QString("Could not open or parse gltf file.");
	}

	/*cout << "Materials: " << avocado.materials.size() << endl;
	gltf::Material &m = avocado.materials[0];
	cout << "baseColorFactor: " << m.pbrMetallicRoughness.baseColorFactor[0] <<
			 m.pbrMetallicRoughness.baseColorFactor[1] <<
			 m.pbrMetallicRoughness.baseColorFactor[2] <<
			endl;
	cout << "baseColorTexture: " << m.pbrMetallicRoughness.baseColorTexture.index << endl;

	cout << "metallicRoughnessTexture " << m.pbrMetallicRoughness.metallicRoughnessTexture.index << endl;
	cout << "diffuse: " << m.extensionsAndExtras.dump() << endl;

	cout << avocado.meshes.size() << endl;
	gltf::Mesh &mesh = avocado.meshes[0];
	gltf::Primitive &primitive = mesh.primitives[0];
	gltf::Attributes &attributes = primitive.attributes;
	//string -> index in the accessors.
	avocado.accessors;
	//an array of accessors -> bufferview, component and type and count.
	//remember it might have a bunding box the positions!

	avocado.bufferViews;// => which buffer, which offset, which length (which should the same as number times type etc);

	avocado.buffers; //each one points to file and length */

}

//TODO we might want to read buffers in blocks.
void GltfLoader::cacheVertices() {

	vertices.setElementsPerBlock(1<<20);

	int32_t nvertices = 0;
	for(auto &mesh: doc->meshes) {
		for(auto &primitive: mesh.primitives) {
			//read one accessor at a time and fill the triangle
			auto &accessor = doc->accessors[primitive.attributes["POSITION"]];
			nvertices += accessor.count;
		}
	}

	vertices.resize(nvertices);
	int32_t current_vertex = 0;
	for(auto &mesh: doc->meshes) {
		for(auto &primitive: mesh.primitives) {


			//read one accessor at a time and fill the triangle
			int32_t start_vertex = current_vertex;
			primitive.attributes["vertex_offset"] = start_vertex;

			{
				auto &pos_accessor = doc->accessors[primitive.attributes["POSITION"]];
				gltf::BufferView &view = doc->bufferViews[pos_accessor.bufferView];
				gltf::Buffer &buffer = doc->buffers[view.buffer];


				if(!view.byteStride)
					view.byteStride = 12;

				uint8_t *pos = buffer.data.data() + pos_accessor.byteOffset+ view.byteOffset;
				for(uint32_t i = 0; i < pos_accessor.count; i++) {
					Vertex &v = vertices[start_vertex + i];

					switch(pos_accessor.componentType) {
					case gltf::Accessor::ComponentType::Float : { //float
						float *p = ((float *)pos);
						v.v[0] = p[0] - origin[0];
						v.v[1] = p[1] - origin[1];
						v.v[2] = p[2] - origin[2];
						if(!view.byteStride)
							pos += 12;
						break;
					}
					default:
						throw QString("Only floating point coordinates supported");
					}
					if(view.byteStride)
						pos += view.byteStride;
				}
				current_vertex += pos_accessor.count;
			}

			if(has_colors) {
				auto &color_accessor = doc->accessors[primitive.attributes["COLOR_0"]];
				gltf::BufferView &view = doc->bufferViews[color_accessor.bufferView];
				gltf::Buffer &buffer = doc->buffers[view.buffer];

				if(!view.byteStride) {
					cerr << "TODO byte stride is needed. Fizx this" << endl;
					exit(0);
				}

				uint8_t *pos = buffer.data.data() + color_accessor.byteOffset+ view.byteOffset;
				for(uint32_t i = 0; i < color_accessor.count; i++) {
					if(i >= vertices.size())
						break;
					Vertex &v = vertices[start_vertex + i];
					v.c[3] = 255;
					switch(color_accessor.componentType) {
					case gltf::Accessor::ComponentType::UnsignedByte ://unsigned byte
						v.c[0] = pos[0];
						v.c[1] = pos[1];
						v.c[2] = pos[2];
						if(color_accessor.type == gltf::Accessor::Type::Vec4)
							v.c[3] = pos[3];
						break;
					case gltf::Accessor::ComponentType::Float : {
						float *c = (float *)pos;
						v.c[0] = (uint8_t)floor(c[0]*255.0f);
						v.c[1] = (uint8_t)floor(c[1]*255.0f);
						v.c[2] = (uint8_t)floor(c[2]*255.0f);
						if(color_accessor.type == gltf::Accessor::Type::Vec4)
							v.c[3] = (uint8_t)floor(c[3]*255.0f);

					}
						break;
					default:
						throw QString("ONly floating point  and byte colors supported");
					}
					pos += view.byteStride;
				}
			}

			if(0 && has_normals) {
				//TODO we should use original normals for the lowest level!
				//and try to preserve them going out. This is useful for geometrical stuff.
/*				auto &accessor = doc->accessors[primitive.attributes["NORMAL"]];
				gltf::BufferView &view = doc->bufferViews[accessor.bufferView];
				gltf::Buffer &buffer = doc->buffers[view.buffer];

				if(accessor.componentType != gltf::Accessor::ComponentType::Float)
					throw QString("Only floating point normals supported");

				if(!view.byteStride)
					view.byteStride = 12;

				uint8_t *pos = buffer.data.data() + accessor.byteOffset+ view.byteOffset;
				for(uint32_t i = 0; i < accessor.count; i++) {
					Vertex &v = vertices[start_vertex + i];
					float *n = (float *)pos;
					v.n[0] = n[0];
					v.n[1] = n[1];
					v.n[2] = n[0];
					pos += view.byteStride;
				} */
			}


			if(has_textures) {
				auto &tex_accessor = doc->accessors[primitive.attributes["TEXCOORD_0"]];
				gltf::BufferView &view = doc->bufferViews[tex_accessor.bufferView];
				gltf::Buffer &buffer = doc->buffers[view.buffer];

				if(tex_accessor.componentType != gltf::Accessor::ComponentType::Float)
					throw QString("Only floating point textures supported");

				if(!view.byteStride)
					view.byteStride = 8;

				uint8_t *pos = buffer.data.data() + tex_accessor.byteOffset + view.byteOffset;
				for(uint32_t i = 0; i < tex_accessor.count; i++) {
					Vertex &v = vertices[start_vertex + i];
					float *t = (float *)pos;
					v.t[0] = t[0];
					v.t[1] = t[1];
					pos += view.byteStride;
				}
			}
		}
	}
}


GltfLoader::~GltfLoader() {
	if(doc) delete doc;
}

void GltfLoader::setMaxMemory(quint64 max_memory) {
	vertices.setMaxMemory(max_memory);
}

quint32 GltfLoader::getTriangles(quint32 size, Triangle *buffer) {
	//current_node?!

	if(current_node == 0 && current_primitive == 0 && current_triangle == 0)
		cacheVertices();

	auto &nodes = doc->nodes; //scenes[0].nodes;

	if(current_node >= nodes.size())
		return 0;

	gltf::Node &node = doc->nodes[current_node];//nodes[current_node]];
	if(node.mesh == -1) {
		current_node++;
		return getTriangles(size, buffer);
	}
	//node.matrix
	gltf::Mesh &mesh = doc->meshes[node.mesh];


	if(current_primitive >= mesh.primitives.size()) {
		current_primitive = 0;
		current_node++;
		return getTriangles(size, buffer);
	}

	gltf::Primitive &primitive = mesh.primitives[current_primitive];
	auto &accessor = doc->accessors[primitive.indices];
	uint32_t offset = primitive.attributes["vertex_offset"];

	if(current_triangle >= accessor.count/3) {
		current_triangle = 0;
		current_primitive++;
		return getTriangles(size, buffer);
	}


	gltf::BufferView &view = doc->bufferViews[accessor.bufferView];
	gltf::Buffer &fbuffer = doc->buffers[view.buffer];

	uint32_t toread = std::min(size, accessor.count/3  - current_triangle);
	if(!toread) return 0;
	uint32_t count = 0;
	uint16_t *spos = (uint16_t *)(fbuffer.data.data() + accessor.byteOffset + view.byteOffset);
	uint32_t *ipos = (uint32_t *)(fbuffer.data.data() + accessor.byteOffset + view.byteOffset);

	vcg::Matrix44f m(node.matrix.data());
	m.transposeInPlace();

	for(uint32_t i = 0; i < toread; i++) {
		current_triangle++;

		Triangle &triangle = buffer[count];
		triangle.tex = primitive.material;
		triangle.node = 0;


		for(int k = 0; k < 3; k++) {
			uint32_t v = 0;
			if(accessor.componentType == gltf::Accessor::ComponentType::UnsignedShort)
				v = spos[i*3 + k];
			else if(accessor.componentType == gltf::Accessor::ComponentType::UnsignedInt)
				v = ipos[i*3 + k];
			else
				throw QString("Unsupported type for indexes (only short and int)");

			Vertex vertex = vertices[v + offset];
			//apply matrix;

			vcg::Point3f p(vertex.v);
			p = m*p;
			vertex.v[0] = p[0];
			vertex.v[1] = p[1];
			vertex.v[2] = p[2];
			triangle.vertices[k] = vertex;
		}

		if(triangle.isDegenerate())
			continue;
		count++;
	}
	if(current_triangle == accessor.count/3) {
		current_primitive++;
		current_triangle = 0;
	}
	return count;
}

quint32 GltfLoader::getVertices(quint32 size, Splat *vertex) {
	throw QString("unimplemented!");
}