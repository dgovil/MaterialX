//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//

#include <MaterialXGraphEditor/Graph.h>

#include <MaterialXRenderGlsl/External/Glad/glad.h>
#include <MaterialXFormat/Util.h>

#include <imgui_stdlib.h>
#include <imgui_node_editor_internal.h>
#include <widgets.h>

#include <iostream>

namespace
{

// Based on the dimensions of the dot_color3 node, computed by calling ed::getNodeSize
const ImVec2 DEFAULT_NODE_SIZE = ImVec2(138, 116);

const int DEFAULT_ALPHA = 255;
const int FILTER_ALPHA = 50;

const std::array<std::string, 22> NODE_GROUP_ORDER =
{
    "texture2d",
    "texture3d",
    "procedural",
    "procedural2d",
    "procedural3d",
    "geometric",
    "translation",
    "convolution2d",
    "math",
    "adjustment",
    "compositing",
    "conditional",
    "channel",
    "organization",
    "global",
    "application",
    "material",
    "shader",
    "pbr",
    "light",
    "colortransform",
    "none"
};

// Based on ImRect_Expanded function in ImGui Node Editor blueprints-example.cpp
ImRect expandImRect(const ImRect& rect, float x, float y)
{
    ImRect result = rect;
    result.Min.x -= x;
    result.Min.y -= y;
    result.Max.x += x;
    result.Max.y += y;
    return result;
}

// Based on the splitter function in the ImGui Node Editor blueprints-example.cpp
static bool splitter(bool split_vertically, float thickness, float* size1, float* size2, float min_size1, float min_size2, float splitter_long_axis_size = -1.0f)
{
    using namespace ImGui;
    ImGuiContext& g = *GImGui;
    ImGuiWindow* window = g.CurrentWindow;
    ImGuiID id = window->GetID("##Splitter");
    ImRect bb;
    bb.Min = window->DC.CursorPos + (split_vertically ? ImVec2(*size1, 0.0f) : ImVec2(0.0f, *size1));
    bb.Max = bb.Min + CalcItemSize(split_vertically ? ImVec2(thickness, splitter_long_axis_size) : ImVec2(splitter_long_axis_size, thickness), 0.0f, 0.0f);
    return SplitterBehavior(bb, id, split_vertically ? ImGuiAxis_X : ImGuiAxis_Y, size1, size2, min_size1, min_size2, 0.0f);
}

// Based on showLabel from ImGui Node Editor blueprints-example.cpp
auto showLabel = [](const char* label, ImColor color)
{
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetTextLineHeight());
    auto size = ImGui::CalcTextSize(label);

    auto padding = ImGui::GetStyle().FramePadding;
    auto spacing = ImGui::GetStyle().ItemSpacing;

    ImGui::SetCursorPos(ImGui::GetCursorPos() + ImVec2(spacing.x, -spacing.y));

    auto rectMin = ImGui::GetCursorScreenPos() - padding;
    auto rectMax = ImGui::GetCursorScreenPos() + size + padding;

    auto drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(rectMin, rectMax, color, size.y * 0.15f);
    ImGui::TextUnformatted(label);
};

// Create a more user-friendly node definition name
std::string getUserNodeDefName(const std::string& val)
{
    const std::string ND_PREFIX = "ND_";
    std::string result = val;
    if (mx::stringStartsWith(val, ND_PREFIX))
    {
        result = val.substr(3, val.length());
    }
    return result;
}

static void EnableSRGBCallback(const ImDrawList*, const ImDrawCmd*)
{
    glEnable(GL_FRAMEBUFFER_SRGB);
}
static void DisableSRGBCallback(const ImDrawList*, const ImDrawCmd*)
{
    glDisable(GL_FRAMEBUFFER_SRGB);
}

} // anonymous namespace

//
// Link methods
//

Link::Link() :
    _startAttr(-1),
    _endAttr(-1)
{
    static int nextId = 1;
    _id = nextId++;
}

//
// Graph methods
//

Graph::Graph(const std::string& materialFilename,
             const std::string& meshFilename,
             const mx::FileSearchPath& searchPath,
             const mx::FilePathVec& libraryFolders,
             int viewWidth,
             int viewHeight) :
    _materialFilename(materialFilename),
    _searchPath(searchPath),
    _libraryFolders(libraryFolders),
    _initial(false),
    _delete(false),
    _fileDialogSave(FileDialog::EnterNewFilename),
    _isNodeGraph(false),
    _graphTotalSize(0),
    _popup(false),
    _shaderPopup(false),
    _searchNodeId(-1),
    _addNewNode(false),
    _ctrlClick(false),
    _isCut(false),
    _autoLayout(false),
    _frameCount(INT_MIN),
    _fontScale(1.0f),
    _saveNodePositions(true)
{
    loadStandardLibraries();
    setPinColor();

    // Set up filters load and save
    _mtlxFilter.push_back(".mtlx");
    _geomFilter.push_back(".obj");
    _geomFilter.push_back(".glb");
    _geomFilter.push_back(".gltf");

    _graphDoc = loadDocument(materialFilename);

    _initial = true;
    createNodeUIList(_stdLib);

    if (_graphDoc)
    {
        buildUiBaseGraph(_graphDoc);
        _currGraphElem = _graphDoc;
        _prevUiNode = nullptr;
    }

    // Create a renderer using the initial startup document.
    mx::FilePath captureFilename = "resources/Materials/Examples/example.png";
    std::string envRadianceFilename = "resources/Lights/san_giuseppe_bridge_split.hdr";
    _renderer = std::make_shared<RenderView>(_graphDoc, _stdLib, meshFilename, envRadianceFilename,
                                             _searchPath, viewWidth, viewHeight);
    _renderer->initialize();
    for (const std::string& ext : _renderer->getImageHandler()->supportedExtensions())
    {
        _imageFilter.emplace_back("." + ext);
    }
    _renderer->updateMaterials(nullptr);
    for (const std::string& incl : _renderer->getXincludeFiles())
    {
        _xincludeFiles.insert(incl);
    }
}

mx::ElementPredicate Graph::getElementPredicate() const
{
    return [this](mx::ConstElementPtr elem)
    {
        if (elem->hasSourceUri())
        {
            return (_xincludeFiles.count(elem->getSourceUri()) == 0);
        }
        return true;
    };
}

void Graph::loadStandardLibraries()
{
    // Initialize the standard library.
    try
    {
        _stdLib = mx::createDocument();
        _xincludeFiles = mx::loadLibraries(_libraryFolders, _searchPath, _stdLib);
        if (_xincludeFiles.empty())
        {
            std::cerr << "Could not find standard data libraries on the given search path: " << _searchPath.asString() << std::endl;
        }
    }
    catch (std::exception& e)
    {
        std::cerr << "Failed to load standard data libraries: " << e.what() << std::endl;
        return;
    }
}

mx::DocumentPtr Graph::loadDocument(const mx::FilePath& filename)
{
    mx::FilePathVec libraryFolders = { "libraries" };
    _libraryFolders = libraryFolders;
    mx::XmlReadOptions readOptions;
    readOptions.readXIncludeFunction = [](mx::DocumentPtr doc, const mx::FilePath& filename,
                                          const mx::FileSearchPath& searchPath, const mx::XmlReadOptions* options)
    {
        mx::FilePath resolvedFilename = searchPath.find(filename);
        if (resolvedFilename.exists())
        {
            try
            {
                readFromXmlFile(doc, resolvedFilename, searchPath, options);
            }
            catch (mx::Exception& e)
            {
                std::cerr << "Failed to read include file: " << filename.asString() << ". " <<
                    std::string(e.what()) << std::endl;
            }
        }
        else
        {
            std::cerr << "Include file not found: " << filename.asString() << std::endl;
        }
    };

    mx::DocumentPtr doc = mx::createDocument();
    try
    {
        if (!filename.isEmpty())
        {
            mx::readFromXmlFile(doc, filename, _searchPath, &readOptions);
            doc->setDataLibrary(_stdLib);
            std::string message;
            if (!doc->validate(&message))
            {
                std::cerr << "*** Validation warnings for " << filename.asString() << " ***" << std::endl;
                std::cerr << message << std::endl;
            }

            // Cache the currently loaded file
            _materialFilename = filename;
        }
    }
    catch (mx::Exception& e)
    {
        std::cerr << "Failed to read file: " << filename.asString() << ": \"" <<
            std::string(e.what()) << "\"" << std::endl;
    }
    _graphStack = std::stack<std::vector<UiNodePtr>>();
    _pinStack = std::stack<std::vector<UiPinPtr>>();
    return doc;
}

void Graph::addExtraNodes()
{
    if (!_graphDoc)
    {
        return;
    }

    // Get all types from the doc
    std::vector<std::string> types;
    std::vector<mx::TypeDefPtr> typeDefs = _graphDoc->getTypeDefs();
    types.reserve(typeDefs.size());
    for (auto typeDef : typeDefs)
    {
        types.push_back(typeDef->getName());
    }

    // Add input and output nodes for all types
    const std::set<std::string> emptySet;
    for (const std::string& type : types)
    {
        std::string nodeName = "ND_input_" + type;
        _nodesToAdd.emplace_back(nodeName, type, "input", "Input Nodes", emptySet, emptySet);
        nodeName = "ND_output_" + type;
        _nodesToAdd.emplace_back(nodeName, type, "output", "Output Nodes", emptySet, emptySet);
    }

    // Add group node
    _nodesToAdd.emplace_back("ND_group", "", "group", "Group Nodes", emptySet, emptySet);

    // Add nodegraph node
    _nodesToAdd.emplace_back("ND_nodegraph", "", "nodegraph", "Node Graph", emptySet, emptySet);
}

ed::PinId Graph::getOutputPin(UiNodePtr node, UiNodePtr upNode, UiPinPtr input)
{
    if (upNode->getNodeGraph() != nullptr)
    {
        // For nodegraph need to get the correct output pin according to the names of the output nodes
        mx::OutputPtr output;
        if (input->_pinNode->getNode())
        {
            output = input->_pinNode->getNode()->getConnectedOutput(input->_name);
        }
        else if (input->_pinNode->getNodeGraph())
        {
            output = input->_pinNode->getNodeGraph()->getConnectedOutput(input->_name);
        }

        if (output)
        {
            std::string outName = output->getName();
            for (UiPinPtr outputs : upNode->outputPins)
            {
                if (outputs->_name == outName)
                {
                    return outputs->_pinId;
                }
            }
        }
        return ed::PinId();
    }
    else
    {
        // For node need to get the correct output pin based on the output attribute
        if (!upNode->outputPins.empty())
        {
            std::string outputName = mx::EMPTY_STRING;
            if (input->_input)
            {
                outputName = input->_input->getOutputString();
            }
            else if (input->_output)
            {
                outputName = input->_output->getOutputString();
            }

            size_t pinIndex = 0;
            if (!outputName.empty())
            {
                for (size_t i = 0; i < upNode->outputPins.size(); i++)
                {
                    if (upNode->outputPins[i]->_name == outputName)
                    {
                        pinIndex = i;
                        break;
                    }
                }
            }
            return (upNode->outputPins[pinIndex]->_pinId);
        }
        return ed::PinId();
    }
}

void Graph::linkGraph()
{
    _currLinks.clear();

    // Start with bottom of graph
    for (UiNodePtr node : _graphNodes)
    {
        std::vector<UiPinPtr> inputs = node->inputPins;
        if (node->getInput() == nullptr)
        {
            for (size_t i = 0; i < inputs.size(); i++)
            {
                // Get upstream node for all inputs
                std::string inputName = inputs[i]->_name;

                UiNodePtr inputNode = node->getConnectedNode(inputName);
                if (inputNode != nullptr)
                {
                    Link link;

                    // Get the input connections for the current UiNode
                    ax::NodeEditor::PinId id = inputs[i]->_pinId;
                    inputs[i]->setConnected(true);
                    int end = int(id.Get());
                    link._endAttr = end;

                    // Get id number of output of node
                    ed::PinId outputId = getOutputPin(node, inputNode, inputs[i]);
                    int start = int(outputId.Get());

                    if (start >= 0)
                    {
                        // Connect the correct output pin to this input
                        for (UiPinPtr outPin : inputNode->outputPins)
                        {
                            if (outPin->_pinId == outputId)
                            {
                                outPin->setConnected(true);
                                outPin->addConnection(inputs[i]);
                            }
                        }

                        link._startAttr = start;

                        if (!linkExists(link))
                        {
                            _currLinks.push_back(link);
                        }
                    }
                }
                else if (inputs[i]->_input)
                {
                    if (inputs[i]->_input->getInterfaceInput())
                    {

                        inputs[i]->setConnected(true);
                    }
                }
                else
                {
                    inputs[i]->setConnected(false);
                }
            }
        }
    }
}

void Graph::connectLinks()
{
    for (Link const& link : _currLinks)
    {
        ed::Link(link._id, link._startAttr, link._endAttr);
    }
}

int Graph::findLinkPosition(int id)
{
    int count = 0;
    for (size_t i = 0; i < _currLinks.size(); i++)
    {
        if (_currLinks[i]._id == id)
        {
            return count;
        }
        count++;
    }
    return -1;
}

bool Graph::checkPosition(UiNodePtr node)
{
    mx::ElementPtr elem = node->getElement();
    return elem && !elem->getAttribute(mx::Element::XPOS_ATTRIBUTE).empty();
}

// Calculate the total vertical space the node level takes up
float Graph::totalHeight(int level)
{
    float total = 0.f;
    for (UiNodePtr node : _levelMap[level])
    {
        total += ed::GetNodeSize(node->getId()).y;
    }
    return total;
}

// Set the y-position of node based on the starting position and the nodes above it
void Graph::setYSpacing(int level, float startingPos)
{
    // set the y spacing for each node
    float currPos = startingPos;
    for (UiNodePtr node : _levelMap[level])
    {
        ImVec2 oldPos = ed::GetNodePosition(node->getId());
        ed::SetNodePosition(node->getId(), ImVec2(oldPos.x, currPos));
        currPos += ed::GetNodeSize(node->getId()).y + 40;
    }
}

// Calculate the average y-position for a specific node level
float Graph::findAvgY(const std::vector<UiNodePtr>& nodes)
{
    // find the mid point of node level grou[
    float total = 0.f;
    int count = 0;
    for (UiNodePtr node : nodes)
    {
        ImVec2 pos = ed::GetNodePosition(node->getId());
        ImVec2 size = ed::GetNodeSize(node->getId());

        total += ((size.y + pos.y) + pos.y) / 2;
        count++;
    }
    return (total / count);
}

void Graph::findYSpacing(float startY)
{
    // Assume level 0 is set
    // For each level find the average y position of the previous level to use as a spacing guide
    int i = 0;
    for (std::pair<int, std::vector<UiNodePtr>> levelChunk : _levelMap)
    {
        if (_levelMap[i].size() > 0)
        {
            if (_levelMap[i][0]->_level > 0)
            {
                int prevLevel = _levelMap[i].front()->_level - 1;
                float avgY = findAvgY(_levelMap[prevLevel]);
                float height = totalHeight(_levelMap[i].front()->_level);
                // calculate the starting position to be above the previous level's center so that it is evenly spaced on either side of the center
                float startingPos = avgY - ((height + (_levelMap[i].size() * 20)) / 2) + startY;
                setYSpacing(_levelMap[i].front()->_level, startingPos);
            }
            else
            {
                setYSpacing(_levelMap[i].front()->_level, startY);
            }
        }
        ++i;
    }
}

ImVec2 Graph::layoutPosition(UiNodePtr layoutNode, ImVec2 startingPos, bool initialLayout, int level)
{
    if (checkPosition(layoutNode) && !_autoLayout)
    {
        for (UiNodePtr node : _graphNodes)
        {
            // Since nodegraph nodes do not have MaterialX info they are placed based on their connected node
            if (node->getNodeGraph() != nullptr)
            {
                std::vector<UiNodePtr> outputCon = node->getOutputConnections();
                if (outputCon.size() > 0)
                {
                    ImVec2 outputPos = ed::GetNodePosition(outputCon[0]->getId());
                    ed::SetNodePosition(node->getId(), ImVec2(outputPos.x - 400, outputPos.y));
                    node->setPos(ImVec2(outputPos.x - 400, outputPos.y));
                }
            }
            else
            {
                // Don't set position of group nodes
                if (node->getMessage().empty())
                {
                    mx::ElementPtr elem = node->getElement();
                    if (elem && elem->hasAttribute(mx::Element::XPOS_ATTRIBUTE))
                    {
                        float x = std::stof(elem->getAttribute(mx::Element::XPOS_ATTRIBUTE));
                        if (elem->hasAttribute(mx::Element::YPOS_ATTRIBUTE))
                        {
                            float y = std::stof(elem->getAttribute(mx::Element::YPOS_ATTRIBUTE));
                            x *= DEFAULT_NODE_SIZE.x;
                            y *= DEFAULT_NODE_SIZE.y;
                            ed::SetNodePosition(node->getId(), ImVec2(x, y));
                            node->setPos(ImVec2(x, y));
                        }
                    }
                }
            }
        }
        return ImVec2(0.f, 0.f);
    }
    else
    {
        ImVec2 currPos = startingPos;
        ImVec2 newPos = currPos;
        if (layoutNode->_level != -1)
        {
            if (layoutNode->_level < level)
            {
                // Remove the old instance of the node from the map
                int levelNum = 0;
                int removeNum = -1;
                for (UiNodePtr levelNode : _levelMap[layoutNode->_level])
                {
                    if (levelNode->getName() == layoutNode->getName())
                    {
                        removeNum = levelNum;
                    }
                    levelNum++;
                }
                if (removeNum > -1)
                {
                    _levelMap[layoutNode->_level].erase(_levelMap[layoutNode->_level].begin() + removeNum);
                }

                layoutNode->_level = level;
            }
        }
        else
        {
            layoutNode->_level = level;
        }

        auto it = _levelMap.find(layoutNode->_level);
        if (it != _levelMap.end())
        {
            // Key already exists so add to it
            bool nodeFound = false;
            for (UiNodePtr node : it->second)
            {
                if (node && node->getName() == layoutNode->getName())
                {
                    nodeFound = true;
                    break;
                }
            }
            if (!nodeFound)
            {
                _levelMap[layoutNode->_level].push_back(layoutNode);
            }
        }
        else
        {
            // Insert new vector into key
            std::vector<UiNodePtr> newValue = { layoutNode };
            _levelMap.emplace(layoutNode->_level, newValue);
        }
        std::vector<UiPinPtr> pins = layoutNode->inputPins;
        if (initialLayout)
        {
            // Check number of inputs that are connected to node
            if (layoutNode->getInputConnect() > 0)
            {
                // Not top of node graph so stop recursion
                if (pins.size() != 0 && layoutNode->getInput() == nullptr)
                {
                    for (size_t i = 0; i < pins.size(); i++)
                    {
                        // Get upstream node for all inputs
                        newPos = startingPos;
                        UiNodePtr nextNode = layoutNode->getConnectedNode(pins[i]->_name);
                        if (nextNode)
                        {
                            startingPos.x = (1200.f - ((layoutNode->_level) * 250)) * _fontScale;
                            ed::SetNodePosition(layoutNode->getId(), startingPos);
                            layoutNode->setPos(ImVec2(startingPos));

                            // Call layout position on upstream node with newPos to the left of current node
                            layoutPosition(nextNode, ImVec2(newPos.x, startingPos.y), initialLayout, layoutNode->_level + 1);
                        }
                    }
                }
            }
            else
            {
                startingPos.x = (1200.f - ((layoutNode->_level) * 250)) * _fontScale;
                layoutNode->setPos(ImVec2(startingPos));

                // Set current node position
                ed::SetNodePosition(layoutNode->getId(), ImVec2(startingPos));
            }
        }
        return ImVec2(0.f, 0.f);
    }
}

void Graph::layoutInputs()
{
    // Layout inputs after other nodes so that they can be all in a line on far left side of node graph
    if (_levelMap.begin() != _levelMap.end())
    {
        int levelCount = -1;
        for (std::pair<int, std::vector<UiNodePtr>> nodes : _levelMap)
        {
            ++levelCount;
        }
        ImVec2 startingPos = ed::GetNodePosition(_levelMap[levelCount].back()->getId());
        startingPos.y += ed::GetNodeSize(_levelMap[levelCount].back()->getId()).y + 20;

        for (UiNodePtr uiNode : _graphNodes)
        {
            if (uiNode->getOutputConnections().size() == 0 && (uiNode->getInput() != nullptr))
            {
                ed::SetNodePosition(uiNode->getId(), ImVec2(startingPos));
                startingPos.y += ed::GetNodeSize(uiNode->getId()).y;
                startingPos.y += 23;
            }
            else if (uiNode->getOutputConnections().size() == 0 && (uiNode->getNode() != nullptr))
            {
                if (uiNode->getNode()->getCategory() != mx::SURFACE_MATERIAL_NODE_STRING)
                {
                    layoutPosition(uiNode, ImVec2(1200, 750), _initial, 0);
                }
            }
        }
    }
}

void Graph::setPinColor()
{
    _pinColor.emplace("integer", ImColor(255, 255, 28, 255));
    _pinColor.emplace("boolean", ImColor(255, 0, 255, 255));
    _pinColor.emplace("float", ImColor(50, 100, 255, 255));
    _pinColor.emplace("color3", ImColor(178, 34, 34, 255));
    _pinColor.emplace("color4", ImColor(50, 10, 255, 255));
    _pinColor.emplace("vector2", ImColor(100, 255, 100, 255));
    _pinColor.emplace("vector3", ImColor(0, 255, 0, 255));
    _pinColor.emplace("vector4", ImColor(100, 0, 100, 255));
    _pinColor.emplace("matrix33", ImColor(0, 100, 100, 255));
    _pinColor.emplace("matrix44", ImColor(50, 255, 100, 255));
    _pinColor.emplace("filename", ImColor(255, 184, 28, 255));
    _pinColor.emplace("string", ImColor(100, 100, 50, 255));
    _pinColor.emplace("geomname", ImColor(121, 60, 180, 255));
    _pinColor.emplace("BSDF", ImColor(10, 181, 150, 255));
    _pinColor.emplace("EDF", ImColor(255, 50, 100, 255));
    _pinColor.emplace("VDF", ImColor(0, 100, 151, 255));
    _pinColor.emplace(mx::SURFACE_SHADER_TYPE_STRING, ImColor(150, 255, 255, 255));
    _pinColor.emplace(mx::MATERIAL_TYPE_STRING, ImColor(255, 255, 255, 255));
    _pinColor.emplace(mx::DISPLACEMENT_SHADER_TYPE_STRING, ImColor(155, 50, 100, 255));
    _pinColor.emplace(mx::VOLUME_SHADER_TYPE_STRING, ImColor(155, 250, 100, 255));
    _pinColor.emplace(mx::LIGHT_SHADER_TYPE_STRING, ImColor(100, 150, 100, 255));
    _pinColor.emplace("none", ImColor(140, 70, 70, 255));
    _pinColor.emplace(mx::MULTI_OUTPUT_TYPE_STRING, ImColor(70, 70, 70, 255));
    _pinColor.emplace("integerarray", ImColor(200, 10, 100, 255));
    _pinColor.emplace("floatarray", ImColor(25, 250, 100));
    _pinColor.emplace("color3array", ImColor(25, 200, 110));
    _pinColor.emplace("color4array", ImColor(50, 240, 110));
    _pinColor.emplace("vector2array", ImColor(50, 200, 75));
    _pinColor.emplace("vector3array", ImColor(20, 200, 100));
    _pinColor.emplace("vector4array", ImColor(100, 200, 100));
    _pinColor.emplace("geomnamearray", ImColor(150, 200, 100));
    _pinColor.emplace("stringarray", ImColor(120, 180, 100));
}

void Graph::setRenderMaterial(UiNodePtr node)
{
    // For now only surface shaders and materials are considered renderable.
    // This can be adjusted as desired to include being able to use outputs,
    // and / a sub-graph in the nodegraph.
    const mx::StringSet RENDERABLE_TYPES = { mx::MATERIAL_TYPE_STRING, mx::SURFACE_SHADER_TYPE_STRING };

    // Set render node right away is node is renderable
    if (node->getNode() && RENDERABLE_TYPES.count(node->getNode()->getType()))
    {
        // Only set new render node if different material has been selected
        if (_currRenderNode != node)
        {
            _currRenderNode = node;
            _frameCount = ImGui::GetFrameCount();
            _renderer->setMaterialCompilation(true);
        }
    }

    // Traverse downstream looking for the first renderable element.
    else
    {
        mx::NodePtr mtlxNode = node->getNode();
        mx::NodeGraphPtr mtlxNodeGraph = node->getNodeGraph();
        mx::OutputPtr mtlxOutput = node->getOutput();
        if (mtlxOutput)
        {
            mx::ElementPtr parent = mtlxOutput->getParent();
            if (parent->isA<mx::NodeGraph>())
                mtlxNodeGraph = parent->asA<mx::NodeGraph>();
            else if (parent->isA<mx::Node>())
                mtlxNode = parent->asA<mx::Node>();
        }
        mx::StringSet testPaths;
        if (mtlxNode)
        {
            mx::ElementPtr parent = mtlxNode->getParent();
            if (parent->isA<mx::NodeGraph>())
            {
                // There is no logic to support traversing from inside a functional graph
                // to it's instance and hence downstream so skip this from consideration.
                // The closest approach would be to "flatten" all definitions to compound graphs.
                mx::NodeGraphPtr parentGraph = parent->asA<mx::NodeGraph>();
                if (parentGraph->getNodeDef())
                {
                    return;
                }
            }
            testPaths.insert(mtlxNode->getNamePath());
        }
        else if (mtlxNodeGraph)
        {
            testPaths.insert(mtlxNodeGraph->getNamePath());
        }

        mx::NodePtr foundNode = nullptr;
        while (!testPaths.empty() && !foundNode)
        {
            mx::StringSet nextPaths;
            for (const std::string& testPath : testPaths)
            {
                mx::ElementPtr testElem = _graphDoc->getDescendant(testPath);
                mx::NodePtr testNode = testElem->asA<mx::Node>();
                std::vector<mx::PortElementPtr> downstreamPorts;
                if (testNode)
                {
                    downstreamPorts = testNode->getDownstreamPorts();
                }
                else
                {
                    mx::NodeGraphPtr testGraph = testElem->asA<mx::NodeGraph>();
                    if (testGraph)
                    {
                        downstreamPorts = testGraph->getDownstreamPorts();
                    }
                }

                // Test all downstream ports. If the port's node is renderable
                // then stop searching.
                for (mx::PortElementPtr downstreamPort : downstreamPorts)
                {
                    mx::ElementPtr parent = downstreamPort->getParent();
                    if (parent)
                    {
                        mx::NodePtr downstreamNode = parent->asA<mx::Node>();
                        if (downstreamNode)
                        {
                            mx::NodeDefPtr nodeDef = downstreamNode->getNodeDef();
                            if (nodeDef)
                            {
                                if (RENDERABLE_TYPES.count(nodeDef->getType()))
                                {
                                    foundNode = downstreamNode;
                                    break;
                                }
                            }
                        }
                        if (!foundNode)
                        {
                            nextPaths.insert(parent->getNamePath());
                        }
                    }
                }
                if (foundNode)
                {
                    break;
                }
            }

            // Set up next set of nodes to search downstream
            testPaths = nextPaths;
        }

        // Update rendering. If found use that node, otherwise
        // use the current fallback of using the first renderable node.
        if (foundNode)
        {
            for (auto uiNode : _graphNodes)
            {
                if (uiNode->getNode() == foundNode)
                {
                    if (_currRenderNode != uiNode)
                    {
                        _currRenderNode = uiNode;
                        _frameCount = ImGui::GetFrameCount();
                        _renderer->setMaterialCompilation(true);
                    }
                    break;
                }
            }
        }
        else
        {
            _currRenderNode = nullptr;
            _frameCount = ImGui::GetFrameCount();
            _renderer->setMaterialCompilation(true);
        }
    }
}

void Graph::updateMaterials(mx::InputPtr input /* = nullptr */, mx::ValuePtr value /* = nullptr */)
{
    std::string renderablePath;
    if (_currRenderNode)
    {
        if (_currRenderNode->getNode())
        {
            renderablePath = _currRenderNode->getNode()->getNamePath();
        }
        else if (_currRenderNode->getOutput())
        {
            renderablePath = _currRenderNode->getOutput()->getNamePath();
        }
    }

    if (renderablePath.empty())
    {
        _renderer->updateMaterials(nullptr);
    }
    else
    {
        if (!input)
        {
            mx::ElementPtr elem = nullptr;
            {
                elem = _graphDoc->getDescendant(renderablePath);
            }
            mx::TypedElementPtr typedElem = elem ? elem->asA<mx::TypedElement>() : nullptr;
            _renderer->updateMaterials(typedElem);
        }
        else
        {
            std::string name = input->getNamePath();

            // Note that if there is a topogical change due to
            // this value change or a transparency change, then
            // this is not currently caught here.
            _renderer->getMaterials()[0]->modifyUniform(name, value);
        }
    }
}

void Graph::setConstant(UiNodePtr node, mx::InputPtr& input, const mx::UIProperties& uiProperties)
{
    ImGui::PushItemWidth(-1);

    mx::ValuePtr minVal = uiProperties.uiMin;
    mx::ValuePtr maxVal = uiProperties.uiMax;

    // If input is a float set the float slider UI to the value
    if (input->getType() == "float")
    {
        mx::ValuePtr val = input->getValue();

        if (val && val->isA<float>())
        {
            // Update the value to the default for new nodes
            float prev, temp;
            prev = temp = val->asA<float>();
            float min = minVal ? minVal->asA<float>() : 0.f;
            float max = maxVal ? maxVal->asA<float>() : 100.f;
            float speed = (max - min) / 1000.0f;
            ImGui::DragFloat("##hidelabel", &temp, speed, min, max);

            // Set input value and update materials if different from previous value
            if (prev != temp)
            {
                addNodeInput(_currUiNode, input);
                input->setValue(temp, input->getType());
                updateMaterials(input, input->getValue());
            }
        }
    }
    else if (input->getType() == "integer")
    {
        mx::ValuePtr val = input->getValue();
        if (val && val->isA<int>())
        {
            int prev, temp;
            prev = temp = val->asA<int>();
            int min = minVal ? minVal->asA<int>() : 0;
            int max = maxVal ? maxVal->asA<int>() : 100;
            float speed = (max - min) / 100.0f;
            ImGui::DragInt("##hidelabel", &temp, speed, min, max);

            // Set input value and update materials if different from previous value
            if (prev != temp)
            {
                addNodeInput(_currUiNode, input);
                input->setValue(temp, input->getType());
                updateMaterials(input, input->getValue());
            }
        }
    }
    else if (input->getType() == "color3")
    {
        mx::ValuePtr val = input->getValue();
        if (val && val->isA<mx::Color3>())
        {
            mx::Color3 prev, temp;

            // Read material value in converted display space
            prev = temp = val->asA<mx::Color3>().linearToSrgb();

            // Use ImGuiColorEditFlags_Uint8 flag for built-in Uint8 input fields
            ImGui::ColorEdit3("##color", &temp[0], ImGuiColorEditFlags_Uint8);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                ImGui::SetTooltip("Color is selected and rendered to Viewer in sRGB display space, \nbut written to .mtlx file in linear format.");
            }

            // Set input value and update materials if different from previous value
            if (prev != temp)
            {
                // Convert back to linear color space for writing to material and node input
                mx::Color3 linearCol = temp.srgbToLinear();

                addNodeInput(_currUiNode, input);
                input->setValue(linearCol, input->getType());

                mx::ValuePtr linearVal = mx::Value::createValue<mx::Color3>(linearCol);
                updateMaterials(input, input->getValue());
            }
        }
    }
    else if (input->getType() == "color4")
    {
        mx::ValuePtr val = input->getValue();
        if (val && val->isA<mx::Color4>())
        {
            // Read material value and convert RGB components to display space
            mx::Color4 linearCol = val->asA<mx::Color4>();
            mx::Color3 displayCol3 = mx::Color3(linearCol[0], linearCol[1], linearCol[2]).linearToSrgb();

            mx::Color4 prev, temp;
            // Create 4D vector with converted RGB and non-converted, stored Alpha value
            prev = temp = mx::Color4(displayCol3[0], displayCol3[1], displayCol3[2], linearCol[3]);

            // Use ImGuiColorEditFlags_Uint8 flag for built-in Uint8 input fields
            ImGui::ColorEdit4("##color", &temp[0], ImGuiColorEditFlags_Uint8);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                ImGui::SetTooltip("Color is selected and rendered to Viewer in sRGB display space, \nbut written to .mtlx file in linear format.");
            }

            // Set input value and update materials if different from previous value
            if (temp != prev)
            {
                // Convert back to linear color space for writing to material and node input
                mx::Color3 linearCol3 = mx::Color3(temp[0], temp[1], temp[2]).srgbToLinear();
                mx::Color4 linearCol = mx::Color4(linearCol3[0], linearCol3[1], linearCol3[2], temp[3]); // Use new Alpha val

                addNodeInput(_currUiNode, input);
                input->setValue(linearCol, input->getType());

                mx::ValuePtr linearVal = mx::Value::createValue<mx::Color4>(linearCol);
                updateMaterials(input, linearVal);
            }
        }
    }
    else if (input->getType() == "vector2")
    {
        mx::ValuePtr val = input->getValue();
        if (val && val->isA<mx::Vector2>())
        {
            mx::Vector2 prev, temp;
            prev = temp = val->asA<mx::Vector2>();
            float min = minVal ? minVal->asA<mx::Vector2>()[0] : 0.f;
            float max = maxVal ? maxVal->asA<mx::Vector2>()[0] : 100.f;
            float speed = (max - min) / 1000.0f;
            ImGui::DragFloat2("##hidelabel", &temp[0], speed, min, max);

            // Set input value and update materials if different from previous value
            if (prev != temp)
            {
                addNodeInput(_currUiNode, input);
                input->setValue(temp, input->getType());
                updateMaterials(input, input->getValue());
            }
        }
    }
    else if (input->getType() == "vector3")
    {
        mx::ValuePtr val = input->getValue();
        if (val && val->isA<mx::Vector3>())
        {
            mx::Vector3 prev, temp;
            prev = temp = val->asA<mx::Vector3>();
            float min = minVal ? minVal->asA<mx::Vector3>()[0] : 0.f;
            float max = maxVal ? maxVal->asA<mx::Vector3>()[0] : 100.f;
            float speed = (max - min) / 1000.0f;
            ImGui::DragFloat3("##hidelabel", &temp[0], speed, min, max);

            // Set input value and update materials if different from previous value
            if (prev != temp)
            {
                addNodeInput(_currUiNode, input);
                input->setValue(temp, input->getType());
                updateMaterials(input, input->getValue());
            }
        }
    }
    else if (input->getType() == "vector4")
    {
        mx::ValuePtr val = input->getValue();
        if (val && val->isA<mx::Vector4>())
        {
            mx::Vector4 prev, temp;
            prev = temp = val->asA<mx::Vector4>();
            float min = minVal ? minVal->asA<mx::Vector4>()[0] : 0.f;
            float max = maxVal ? maxVal->asA<mx::Vector4>()[0] : 100.f;
            float speed = (max - min) / 1000.0f;
            ImGui::DragFloat4("##hidelabel", &temp[0], speed, min, max);

            // Set input value and update materials if different from previous value
            if (prev != temp)
            {
                addNodeInput(_currUiNode, input);
                input->setValue(temp, input->getType());
                updateMaterials(input, input->getValue());
            }
        }
    }
    else if (input->getType() == "string")
    {
        mx::ValuePtr val = input->getValue();
        if (val && val->isA<std::string>())
        {
            std::string prev, temp;
            prev = temp = val->asA<std::string>();
            ImGui::InputText("##constant", &temp);

            // Set input value and update materials if different from previous value
            if (prev != temp)
            {
                addNodeInput(_currUiNode, input);
                input->setValue(temp, input->getType());
                updateMaterials();
            }
        }
    }
    else if (input->getType() == "filename")
    {
        mx::ValuePtr val = input->getValue();

        if (val && val->isA<std::string>())
        {
            std::string prev, temp;
            prev = temp = val->asA<std::string>();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.15f, .15f, .15f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.2f, .4f, .6f, 1.0f));

            // Browser button to select new file
            ImGui::PushItemWidth(-100);
            if (ImGui::Button("Browse"))
            {
                _fileDialogImageInputName = input->getName();
                _fileDialogImage.setTitle("Node Input Dialog");
                _fileDialogImage.open();
                _fileDialogImage.setTypeFilters(_imageFilter);
            }
            ImGui::PopItemWidth();
            ImGui::SameLine();
            ImGui::Text("%s", mx::FilePath(temp).getBaseName().c_str());
            ImGui::PopStyleColor();
            ImGui::PopStyleColor();

            // Create and load document from selected file
            if (_fileDialogImage.hasSelected() && _fileDialogImageInputName == input->getName())
            {
                // Set the new filename to the complete file path
                mx::FilePath fileName = _fileDialogImage.getSelected();
                temp = fileName;

                // Need to clear the file prefix so that it can find the new file
                input->setFilePrefix(mx::EMPTY_STRING);
                _fileDialogImage.clearSelected();
                _fileDialogImage.setTypeFilters(std::vector<std::string>());
                _fileDialogImageInputName = "";
            }

            // Set input value and update materials if different from previous value
            if (prev != temp)
            {
                addNodeInput(_currUiNode, input);
                input->setValueString(temp);
                input->setValue(temp, input->getType());
                updateMaterials();
            }
        }
    }
    else if (input->getType() == "boolean")
    {
        mx::ValuePtr val = input->getValue();
        if (val && val->isA<bool>())
        {
            bool prev, temp;
            prev = temp = val->asA<bool>();
            ImGui::Checkbox("", &temp);

            // Set input value and update materials if different from previous value
            if (prev != temp)
            {
                addNodeInput(_currUiNode, input);
                input->setValue(temp, input->getType());
                updateMaterials(input, input->getValue());
            }
        }
    }

    ImGui::PopItemWidth();
}

void Graph::setUiNodeInfo(UiNodePtr node, const std::string& type, const std::string& category)
{
    node->setType(type);
    node->setCategory(category);
    ++_graphTotalSize;

    // Create pins
    if (node->getNodeGraph())
    {
        std::vector<mx::OutputPtr> outputs = node->getNodeGraph()->getOutputs();
        for (mx::OutputPtr out : outputs)
        {
            UiPinPtr outPin = std::make_shared<UiPin>(_graphTotalSize, &*out->getName().begin(), out->getType(), node, ax::NodeEditor::PinKind::Output, nullptr, nullptr);
            ++_graphTotalSize;
            node->outputPins.push_back(outPin);
            _currPins.push_back(outPin);
        }

        for (mx::InputPtr input : node->getNodeGraph()->getInputs())
        {
            UiPinPtr inPin = std::make_shared<UiPin>(_graphTotalSize, &*input->getName().begin(), input->getType(), node, ax::NodeEditor::PinKind::Input, input, nullptr);
            node->inputPins.push_back(inPin);
            _currPins.push_back(inPin);
            ++_graphTotalSize;
        }
    }
    else
    {
        if (node->getNode())
        {
            mx::NodeDefPtr nodeDef = node->getNode()->getNodeDef(node->getNode()->getName());
            if (nodeDef)
            {
                for (mx::InputPtr input : nodeDef->getActiveInputs())
                {
                    if (node->getNode()->getInput(input->getName()))
                    {
                        input = node->getNode()->getInput(input->getName());
                    }
                    UiPinPtr inPin = std::make_shared<UiPin>(_graphTotalSize, &*input->getName().begin(), input->getType(), node, ax::NodeEditor::PinKind::Input, input, nullptr);
                    node->inputPins.push_back(inPin);
                    _currPins.push_back(inPin);
                    ++_graphTotalSize;
                }

                for (mx::OutputPtr output : nodeDef->getActiveOutputs())
                {
                    if (node->getNode()->getOutput(output->getName()))
                    {
                        output = node->getNode()->getOutput(output->getName());
                    }
                    UiPinPtr outPin = std::make_shared<UiPin>(_graphTotalSize, &*output->getName().begin(), output->getType(),
                                                              node, ax::NodeEditor::PinKind::Output, nullptr, nullptr);
                    node->outputPins.push_back(outPin);
                    _currPins.push_back(outPin);
                    ++_graphTotalSize;
                }
            }
        }
        else if (node->getInput())
        {
            UiPinPtr inPin = std::make_shared<UiPin>(_graphTotalSize, &*("Value"), node->getInput()->getType(), node, ax::NodeEditor::PinKind::Input, node->getInput(), nullptr);
            node->inputPins.push_back(inPin);
            _currPins.push_back(inPin);
            ++_graphTotalSize;
        }
        else if (node->getOutput())
        {
            UiPinPtr inPin = std::make_shared<UiPin>(_graphTotalSize, &*("input"), node->getOutput()->getType(), node, ax::NodeEditor::PinKind::Input, nullptr, node->getOutput());
            node->inputPins.push_back(inPin);
            _currPins.push_back(inPin);
            ++_graphTotalSize;
        }

        if (node->getInput() || node->getOutput())
        {
            UiPinPtr outPin = std::make_shared<UiPin>(_graphTotalSize, &*("output"), type, node, ax::NodeEditor::PinKind::Output, nullptr, nullptr);
            ++_graphTotalSize;
            node->outputPins.push_back(outPin);
            _currPins.push_back(outPin);
        }
    }

    _graphNodes.push_back(std::move(node));
}

void Graph::createNodeUIList(mx::DocumentPtr doc)
{
    _nodesToAdd.clear();

    auto nodeDefs = doc->getNodeDefs();
    std::unordered_map<std::string, std::vector<mx::NodeDefPtr>> groupToNodeDef;
    std::vector<std::string> groupList = std::vector(NODE_GROUP_ORDER.begin(), NODE_GROUP_ORDER.end());

    for (const auto& nodeDef : nodeDefs)
    {
        std::string group = nodeDef->getNodeGroup();
        if (group.empty())
        {
            group = NODE_GROUP_ORDER.back();
        }

        // If the group is not in the groupList already (seeded by NODE_GROUP_ORDER) then add it.
        if (std::find(groupList.begin(), groupList.end(), group) == groupList.end())
        {
            groupList.emplace_back(group);
        }

        if (groupToNodeDef.find(group) == groupToNodeDef.end())
        {
            groupToNodeDef[group] = std::vector<mx::NodeDefPtr>();
        }
        groupToNodeDef[group].push_back(nodeDef);
    }

    for (const auto& group : groupList)
    {
        auto it = groupToNodeDef.find(group);
        if (it != groupToNodeDef.end())
        {
            const auto& groupNodeDefs = it->second;

            for (const auto& nodeDef : groupNodeDefs)
            {
                std::set<std::string> inputTypes;
                for (const auto& input : nodeDef->getActiveInputs())
                {
                    inputTypes.insert(input->getType());
                }
                std::set<std::string> outputTypes;
                for (const auto& output : nodeDef->getOutputs())
                {
                    outputTypes.insert(output->getType());
                }
                _nodesToAdd.emplace_back(nodeDef->getName(), nodeDef->getType(), nodeDef->getNodeString(), group, inputTypes, outputTypes);
            }
        }
    }

    addExtraNodes();
}

void Graph::buildUiBaseGraph(mx::DocumentPtr doc)
{
    std::vector<mx::NodeGraphPtr> nodeGraphs = doc->getNodeGraphs();
    std::vector<mx::InputPtr> inputNodes = doc->getActiveInputs();
    std::vector<mx::OutputPtr> outputNodes = doc->getOutputs();
    std::vector<mx::NodePtr> docNodes = doc->getNodes();

    mx::ElementPredicate includeElement = getElementPredicate();

    _graphNodes.clear();
    _currLinks.clear();
    _currEdge.clear();
    _newLinks.clear();
    _currPins.clear();
    _graphTotalSize = 1;

    // Create UiNodes for nodes that belong to the document so they are not in a nodegraph
    for (mx::NodePtr node : docNodes)
    {
        if (!includeElement(node))
            continue;
        std::string name = node->getName();
        auto currNode = std::make_shared<UiNode>(name, _graphTotalSize);
        currNode->setNode(node);
        setUiNodeInfo(currNode, node->getType(), node->getCategory());
    }

    // Create UiNodes for the nodegraph
    for (mx::NodeGraphPtr nodeGraph : nodeGraphs)
    {
        if (!includeElement(nodeGraph))
            continue;
        std::string name = nodeGraph->getName();
        auto currNode = std::make_shared<UiNode>(name, _graphTotalSize);
        currNode->setNodeGraph(nodeGraph);
        setUiNodeInfo(currNode, "", "nodegraph");
    }
    for (mx::InputPtr input : inputNodes)
    {
        if (!includeElement(input))
            continue;
        auto currNode = std::make_shared<UiNode>(input->getName(), _graphTotalSize);
        currNode->setInput(input);
        setUiNodeInfo(currNode, input->getType(), input->getCategory());
    }
    for (mx::OutputPtr output : outputNodes)
    {
        if (!includeElement(output))
            continue;
        auto currNode = std::make_shared<UiNode>(output->getName(), _graphTotalSize);
        currNode->setOutput(output);
        setUiNodeInfo(currNode, output->getType(), output->getCategory());
    }

    // Create edges for nodegraphs
    for (mx::NodeGraphPtr graph : nodeGraphs)
    {
        for (mx::InputPtr input : graph->getActiveInputs())
        {
            int downNum = -1;
            int upNum = -1;
            mx::string nodeGraphName = input->getNodeGraphString();
            mx::NodePtr connectedNode = input->getConnectedNode();
            if (!nodeGraphName.empty())
            {
                downNum = findNode(graph->getName(), "nodegraph");
                upNum = findNode(nodeGraphName, "nodegraph");
            }
            else if (connectedNode)
            {
                downNum = findNode(graph->getName(), "nodegraph");
                upNum = findNode(connectedNode->getName(), "node");
            }

            if (upNum > -1)
            {
                UiEdge newEdge = UiEdge(_graphNodes[upNum], _graphNodes[downNum], input);
                if (!edgeExists(newEdge))
                {
                    _graphNodes[downNum]->edges.push_back(newEdge);
                    _graphNodes[downNum]->setInputNodeNum(1);
                    _graphNodes[upNum]->setOutputConnection(_graphNodes[downNum]);
                    _currEdge.push_back(newEdge);
                }
            }
        }
    }

    // Create edges for surface and material nodes
    for (mx::NodePtr node : docNodes)
    {
        mx::NodeDefPtr nD = node->getNodeDef(node->getName());
        for (mx::InputPtr input : node->getActiveInputs())
        {

            mx::string nodeGraphName = input->getNodeGraphString();
            mx::NodePtr connectedNode = input->getConnectedNode();
            mx::OutputPtr connectedOutput = input->getConnectedOutput();
            int upNum = -1;
            int downNum = -1;
            if (!nodeGraphName.empty())
            {

                upNum = findNode(nodeGraphName, "nodegraph");
                downNum = findNode(node->getName(), "node");
            }
            else if (connectedNode)
            {
                upNum = findNode(connectedNode->getName(), "node");
                downNum = findNode(node->getName(), "node");
            }
            else if (connectedOutput)
            {
                upNum = findNode(connectedOutput->getName(), "output");
                downNum = findNode(node->getName(), "node");
            }
            else if (!input->getInterfaceName().empty())
            {
                upNum = findNode(input->getInterfaceName(), "input");
                downNum = findNode(node->getName(), "node");
            }
            if (upNum != -1)
            {
                UiEdge newEdge = UiEdge(_graphNodes[upNum], _graphNodes[downNum], input);
                if (!edgeExists(newEdge))
                {
                    _graphNodes[downNum]->edges.push_back(newEdge);
                    _graphNodes[downNum]->setInputNodeNum(1);
                    _graphNodes[upNum]->setOutputConnection(_graphNodes[downNum]);
                    _currEdge.push_back(newEdge);
                }
            }
        }
    }
}

void Graph::buildUiNodeGraph(const mx::NodeGraphPtr& nodeGraphs)
{
    // Clear all values so that ids can start with 0 or 1
    _graphNodes.clear();
    _currLinks.clear();
    _currEdge.clear();
    _newLinks.clear();
    _currPins.clear();
    _graphTotalSize = 1;
    if (nodeGraphs)
    {
        mx::NodeGraphPtr nodeGraph = nodeGraphs;
        std::vector<mx::ElementPtr> children = nodeGraph->topologicalSort();
        mx::NodeDefPtr nodeDef = nodeGraph->getNodeDef();
        mx::NodeDefPtr currNodeDef;

        // Create input nodes
        if (nodeDef)
        {
            std::vector<mx::InputPtr> inputs = nodeDef->getActiveInputs();

            for (mx::InputPtr input : inputs)
            {
                auto currNode = std::make_shared<UiNode>(input->getName(), _graphTotalSize);
                currNode->setInput(input);
                setUiNodeInfo(currNode, input->getType(), input->getCategory());
            }
        }

        // Search node graph children to create uiNodes
        for (mx::ElementPtr elem : children)
        {
            mx::NodePtr node = elem->asA<mx::Node>();
            mx::InputPtr input = elem->asA<mx::Input>();
            mx::OutputPtr output = elem->asA<mx::Output>();
            std::string name = elem->getName();
            auto currNode = std::make_shared<UiNode>(name, _graphTotalSize);
            if (node)
            {
                currNode->setNode(node);
                setUiNodeInfo(currNode, node->getType(), node->getCategory());
            }
            else if (input)
            {
                currNode->setInput(input);
                setUiNodeInfo(currNode, input->getType(), input->getCategory());
            }
            else if (output)
            {
                currNode->setOutput(output);
                setUiNodeInfo(currNode, output->getType(), output->getCategory());
            }
        }

        // Write out all connections.
        std::set<mx::Edge> processedEdges;
        for (mx::OutputPtr output : nodeGraph->getOutputs())
        {
            for (mx::Edge edge : output->traverseGraph())
            {
                if (!processedEdges.count(edge))
                {
                    mx::ElementPtr upstreamElem = edge.getUpstreamElement();
                    mx::ElementPtr downstreamElem = edge.getDownstreamElement();
                    mx::ElementPtr connectingElem = edge.getConnectingElement();

                    mx::NodePtr upstreamNode = upstreamElem->asA<mx::Node>();
                    mx::NodePtr downstreamNode = downstreamElem->asA<mx::Node>();
                    mx::InputPtr upstreamInput = upstreamElem->asA<mx::Input>();
                    mx::InputPtr downstreamInput = downstreamElem->asA<mx::Input>();
                    mx::OutputPtr upstreamOutput = upstreamElem->asA<mx::Output>();
                    mx::OutputPtr downstreamOutput = downstreamElem->asA<mx::Output>();
                    std::string downName = downstreamElem->getName();
                    std::string upName = upstreamElem->getName();
                    std::string upstreamType;
                    std::string downstreamType;
                    if (upstreamNode)
                    {
                        upstreamType = "node";
                    }
                    else if (upstreamInput)
                    {
                        upstreamType = "input";
                    }
                    else if (upstreamOutput)
                    {
                        upstreamType = "output";
                    }
                    if (downstreamNode)
                    {
                        downstreamType = "node";
                    }
                    else if (downstreamInput)
                    {
                        downstreamType = "input";
                    }
                    else if (downstreamOutput)
                    {
                        downstreamType = "output";
                    }
                    int upNode = findNode(upName, upstreamType);
                    int downNode = findNode(downName, downstreamType);
                    if (downNode > 0 && upNode > 0 && _graphNodes[downNode]->getOutput())
                    {
                        // Create edges for the output nodes
                        UiEdge newEdge = UiEdge(_graphNodes[upNode], _graphNodes[downNode], nullptr);
                        if (!edgeExists(newEdge))
                        {
                            _graphNodes[downNode]->edges.push_back(newEdge);
                            _graphNodes[downNode]->setInputNodeNum(1);
                            _graphNodes[upNode]->setOutputConnection(_graphNodes[downNode]);
                            _currEdge.push_back(newEdge);
                        }
                    }
                    else if (connectingElem)
                    {

                        mx::InputPtr connectingInput = connectingElem->asA<mx::Input>();

                        if (connectingInput)
                        {
                            if ((upNode >= 0) && (downNode >= 0))
                            {
                                UiEdge newEdge = UiEdge(_graphNodes[upNode], _graphNodes[downNode], connectingInput);
                                if (!edgeExists(newEdge))
                                {
                                    _graphNodes[downNode]->edges.push_back(newEdge);
                                    _graphNodes[downNode]->setInputNodeNum(1);
                                    _graphNodes[upNode]->setOutputConnection(_graphNodes[downNode]);
                                    _currEdge.push_back(newEdge);
                                }
                            }
                        }
                    }
                    if (upstreamNode)
                    {
                        std::vector<mx::InputPtr> ins = upstreamNode->getActiveInputs();
                        for (mx::InputPtr input : ins)
                        {
                            // Connect input nodes
                            if (input->hasInterfaceName())
                            {
                                std::string interfaceName = input->getInterfaceName();
                                int newUp = findNode(interfaceName, "input");
                                if (newUp >= 0)
                                {
                                    mx::InputPtr inputP = std::make_shared<mx::Input>(downstreamElem, input->getName());
                                    UiEdge newEdge = UiEdge(_graphNodes[newUp], _graphNodes[upNode], input);
                                    if (!edgeExists(newEdge))
                                    {
                                        _graphNodes[upNode]->edges.push_back(newEdge);
                                        _graphNodes[upNode]->setInputNodeNum(1);
                                        _graphNodes[newUp]->setOutputConnection(_graphNodes[upNode]);
                                        _currEdge.push_back(newEdge);
                                    }
                                }
                            }
                        }
                    }

                    processedEdges.insert(edge);
                }
            }
        }

        // Second pass to catch all of the connections that arent part of an output
        for (mx::ElementPtr elem : children)
        {
            mx::NodePtr node = elem->asA<mx::Node>();
            mx::InputPtr inputElem = elem->asA<mx::Input>();
            mx::OutputPtr output = elem->asA<mx::Output>();
            if (node)
            {
                std::vector<mx::InputPtr> inputs = node->getActiveInputs();
                for (mx::InputPtr input : inputs)
                {
                    mx::NodePtr upNode = input->getConnectedNode();
                    if (upNode)
                    {
                        int upNum = findNode(upNode->getName(), "node");
                        int downNode = findNode(node->getName(), "node");
                        if ((upNum >= 0) && (downNode >= 0))
                        {

                            UiEdge newEdge = UiEdge(_graphNodes[upNum], _graphNodes[downNode], input);
                            if (!edgeExists(newEdge))
                            {
                                _graphNodes[downNode]->edges.push_back(newEdge);
                                _graphNodes[downNode]->setInputNodeNum(1);
                                _graphNodes[upNum]->setOutputConnection(_graphNodes[downNode]);
                                _currEdge.push_back(newEdge);
                            }
                        }
                    }
                    else if (input->getInterfaceInput())
                    {
                        int upNum = findNode(input->getInterfaceInput()->getName(), "input");
                        int downNode = findNode(node->getName(), "node");
                        if ((upNum >= 0) && (downNode >= 0))
                        {

                            UiEdge newEdge = UiEdge(_graphNodes[upNum], _graphNodes[downNode], input);
                            if (!edgeExists(newEdge))
                            {
                                _graphNodes[downNode]->edges.push_back(newEdge);
                                _graphNodes[downNode]->setInputNodeNum(1);
                                _graphNodes[upNum]->setOutputConnection(_graphNodes[downNode]);
                                _currEdge.push_back(newEdge);
                            }
                        }
                    }
                }
            }
            else if (output)
            {
                mx::NodePtr upNode = output->getConnectedNode();
                if (upNode)
                {
                    int upNum = findNode(upNode->getName(), "node");
                    int downNode = findNode(output->getName(), "output");
                    UiEdge newEdge = UiEdge(_graphNodes[upNum], _graphNodes[downNode], nullptr);
                    if (!edgeExists(newEdge))
                    {
                        _graphNodes[downNode]->edges.push_back(newEdge);
                        _graphNodes[downNode]->setInputNodeNum(1);
                        _graphNodes[upNum]->setOutputConnection(_graphNodes[downNode]);
                        _currEdge.push_back(newEdge);
                    }
                }
            }
        }
    }
}

int Graph::findNode(const std::string& name, const std::string& type)
{
    int count = 0;
    for (size_t i = 0; i < _graphNodes.size(); i++)
    {
        if (_graphNodes[i]->getName() == name)
        {
            if (type == "node" && _graphNodes[i]->getNode() != nullptr)
            {
                return count;
            }
            else if (type == "input" && _graphNodes[i]->getInput() != nullptr)
            {
                return count;
            }
            else if (type == "output" && _graphNodes[i]->getOutput() != nullptr)
            {
                return count;
            }
            else if (type == "nodegraph" && _graphNodes[i]->getNodeGraph() != nullptr)
            {
                return count;
            }
        }
        count++;
    }
    return -1;
}

void Graph::positionPasteBin(ImVec2 pos)
{
    ImVec2 totalPos = ImVec2(0, 0);
    ImVec2 avgPos = ImVec2(0, 0);

    // Get average position of original nodes
    for (auto pasteNode : _copiedNodes)
    {
        ImVec2 origPos = ed::GetNodePosition(pasteNode.first->getId());
        totalPos.x += origPos.x;
        totalPos.y += origPos.y;
    }
    avgPos.x = totalPos.x / (int) _copiedNodes.size();
    avgPos.y = totalPos.y / (int) _copiedNodes.size();

    // Get offset from clicked position
    ImVec2 offset = ImVec2(0, 0);
    offset.x = pos.x - avgPos.x;
    offset.y = pos.y - avgPos.y;
    for (auto pasteNode : _copiedNodes)
    {
        if (!pasteNode.second)
        {
            continue;
        }
        ImVec2 newPos = ImVec2(0, 0);
        newPos.x = ed::GetNodePosition(pasteNode.first->getId()).x + offset.x;
        newPos.y = ed::GetNodePosition(pasteNode.first->getId()).y + offset.y;
        ed::SetNodePosition(pasteNode.second->getId(), newPos);
    }
}

void Graph::createEdge(UiNodePtr upNode, UiNodePtr downNode, mx::InputPtr connectingInput)
{
    if (downNode->getOutput())
    {
        // Create edges for the output nodes
        UiEdge newEdge = UiEdge(upNode, downNode, nullptr);
        if (!edgeExists(newEdge))
        {
            downNode->edges.push_back(newEdge);
            downNode->setInputNodeNum(1);
            upNode->setOutputConnection(downNode);
            _currEdge.push_back(newEdge);
        }
    }
    else if (connectingInput)
    {
        UiEdge newEdge = UiEdge(upNode, downNode, connectingInput);
        downNode->edges.push_back(newEdge);
        downNode->setInputNodeNum(1);
        upNode->setOutputConnection(downNode);
        _currEdge.push_back(newEdge);
    }
}

void Graph::copyUiNode(UiNodePtr node)
{
    UiNodePtr copyNode = std::make_shared<UiNode>(mx::EMPTY_STRING, int(_graphTotalSize + 1));
    ++_graphTotalSize;
    if (node->getElement())
    {
        std::string newName = _currGraphElem->createValidChildName(node->getName());
        if (node->getNode())
        {
            mx::NodePtr mxNode;
            mxNode = _currGraphElem->addNodeInstance(node->getNode()->getNodeDef());
            mxNode->copyContentFrom(node->getNode());
            mxNode->setName(newName);
            copyNode->setNode(mxNode);
        }
        else if (node->getInput())
        {
            mx::InputPtr mxInput;
            mxInput = _currGraphElem->addInput(newName);
            mxInput->copyContentFrom(node->getInput());
            copyNode->setInput(mxInput);
        }
        else if (node->getOutput())
        {
            mx::OutputPtr mxOutput;
            mxOutput = _currGraphElem->addOutput(newName);
            mxOutput->copyContentFrom(node->getOutput());
            mxOutput->setName(newName);
            copyNode->setOutput(mxOutput);
        }
        copyNode->getElement()->setName(newName);
        copyNode->setName(newName);
    }
    else if (node->getNodeGraph())
    {
        _graphDoc->addNodeGraph();
        std::string nodeGraphName = _graphDoc->getNodeGraphs().back()->getName();
        copyNode->setNodeGraph(_graphDoc->getNodeGraphs().back());
        copyNode->setName(nodeGraphName);
        copyNodeGraph(node, copyNode);
    }
    setUiNodeInfo(copyNode, node->getType(), node->getCategory());
    _copiedNodes[node] = copyNode;
    _graphNodes.push_back(copyNode);
}

void Graph::copyNodeGraph(UiNodePtr origGraph, UiNodePtr copyGraph)
{
    copyGraph->getNodeGraph()->copyContentFrom(origGraph->getNodeGraph());
    std::vector<mx::InputPtr> inputs = copyGraph->getNodeGraph()->getActiveInputs();
    for (mx::InputPtr input : inputs)
    {
        std::string newName = _graphDoc->createValidChildName(input->getName());
        input->setName(newName);
    }
}

void Graph::copyInputs()
{
    for (std::map<UiNodePtr, UiNodePtr>::iterator iter = _copiedNodes.begin(); iter != _copiedNodes.end(); ++iter)
    {
        int count = 0;
        UiNodePtr origNode = iter->first;
        UiNodePtr copyNode = iter->second;
        for (UiPinPtr pin : origNode->inputPins)
        {
            if (origNode->getConnectedNode(pin->_name) && !_ctrlClick)
            {
                // If original node is connected check if connect node is in copied nodes
                if (_copiedNodes.find(origNode->getConnectedNode(pin->_name)) != _copiedNodes.end())
                {
                    // Set copy node connected to the value at this key
                    createEdge(_copiedNodes[origNode->getConnectedNode(pin->_name)], copyNode, copyNode->inputPins[count]->_input);
                    UiNodePtr upNode = _copiedNodes[origNode->getConnectedNode(pin->_name)];
                    if (copyNode->getNode() || copyNode->getNodeGraph())
                    {
                        mx::InputPtr connectingInput = nullptr;
                        copyNode->inputPins[count]->_input->copyContentFrom(pin->_input);

                        // Update value to be empty
                        if (copyNode->getNode() && copyNode->getNode()->getType() == mx::SURFACE_SHADER_TYPE_STRING)
                        {
                            if (upNode->getOutput())
                            {
                                copyNode->inputPins[count]->_input->setConnectedOutput(upNode->getOutput());
                            }
                            else if (upNode->getInput())
                            {

                                copyNode->inputPins[count]->_input->setConnectedInterfaceName(upNode->getName());
                            }
                            else
                            {
                                if (upNode->getNodeGraph())
                                {
                                    ed::PinId outputId = getOutputPin(copyNode, upNode, copyNode->inputPins[count]);
                                    for (UiPinPtr outPin : upNode->outputPins)
                                    {
                                        if (outPin->_pinId == outputId)
                                        {
                                            mx::OutputPtr outputs = upNode->getNodeGraph()->getOutput(outPin->_name);
                                            copyNode->inputPins[count]->_input->setConnectedOutput(outputs);
                                        }
                                    }
                                }
                                else
                                {
                                    copyNode->inputPins[count]->_input->setConnectedNode(upNode->getNode());
                                }
                            }
                        }
                        else
                        {
                            if (upNode->getInput())
                            {
                                copyNode->inputPins[count]->_input->setConnectedInterfaceName(upNode->getName());
                            }
                            else
                            {
                                copyNode->inputPins[count]->_input->setConnectedNode(upNode->getNode());
                            }
                        }

                        copyNode->inputPins[count]->setConnected(true);
                    }
                    else if (copyNode->getOutput() != nullptr)
                    {
                        mx::InputPtr connectingInput = nullptr;
                        copyNode->getOutput()->setConnectedNode(upNode->getNode());
                    }

                    // Update input node num and output connections
                    copyNode->setInputNodeNum(1);
                    upNode->setOutputConnection(copyNode);
                }
                else if (pin->_input)
                {
                    if (pin->_input->getInterfaceInput())
                    {
                        copyNode->inputPins[count]->_input->setConnectedInterfaceName(mx::EMPTY_STRING);
                    }
                    copyNode->inputPins[count]->setConnected(false);
                    setDefaults(copyNode->inputPins[count]->_input);
                    copyNode->inputPins[count]->_input->setConnectedNode(nullptr);
                    copyNode->inputPins[count]->_input->setConnectedOutput(nullptr);
                }
            }
            count++;
        }
    }
}

void Graph::addNode(const std::string& category, const std::string& name, const std::string& type)
{
    mx::NodePtr node = nullptr;
    std::vector<mx::NodeDefPtr> matchingNodeDefs;

    // Create document or node graph is there is not already one
    if (category == "output")
    {
        std::string outName = "";
        mx::OutputPtr newOut;
        // add output as child of correct parent and create valid name
        outName = _currGraphElem->createValidChildName(name);
        newOut = _currGraphElem->addOutput(outName, type);
        auto outputNode = std::make_shared<UiNode>(outName, int(++_graphTotalSize));
        outputNode->setOutput(newOut);
        setUiNodeInfo(outputNode, type, category);
        return;
    }
    if (category == "input")
    {
        std::string inName = "";
        mx::InputPtr newIn = nullptr;

        // Add input as child of correct parent and create valid name
        inName = _currGraphElem->createValidChildName(name);
        newIn = _currGraphElem->addInput(inName, type);
        auto inputNode = std::make_shared<UiNode>(inName, int(++_graphTotalSize));
        setDefaults(newIn);
        inputNode->setInput(newIn);
        setUiNodeInfo(inputNode, type, category);
        return;
    }
    else if (category == "group")
    {
        auto groupNode = std::make_shared<UiNode>(name, int(++_graphTotalSize));

        // Set message of group UiNode in order to identify it as such
        groupNode->setMessage("Comment");
        setUiNodeInfo(groupNode, type, "group");

        // Create ui portions of group node
        buildGroupNode(_graphNodes.back());
        return;
    }
    else if (category == "nodegraph")
    {
        // Create new mx::NodeGraph and set as current node graph
        _graphDoc->addNodeGraph();
        std::string nodeGraphName = _graphDoc->getNodeGraphs().back()->getName();
        auto nodeGraphNode = std::make_shared<UiNode>(nodeGraphName, int(++_graphTotalSize));

        // Set mx::Nodegraph as node graph for uiNode
        nodeGraphNode->setNodeGraph(_graphDoc->getNodeGraphs().back());

        setUiNodeInfo(nodeGraphNode, type, "nodegraph");
        return;
    }
    else
    {
        matchingNodeDefs = _graphDoc->getMatchingNodeDefs(category);
        for (mx::NodeDefPtr nodedef : matchingNodeDefs)
        {
            std::string userNodeDefName = getUserNodeDefName(nodedef->getName());
            if (userNodeDefName == name)
            {
                node = _currGraphElem->addNodeInstance(nodedef, _currGraphElem->createValidChildName(name));
            }
        }
    }

    if (node)
    {
        int num = 0;
        int countDef = 0;
        for (size_t i = 0; i < matchingNodeDefs.size(); i++)
        {
            std::string userNodeDefName = getUserNodeDefName(matchingNodeDefs[i]->getName());
            if (userNodeDefName == name)
            {
                num = countDef;
            }
            countDef++;
        }
        std::vector<mx::InputPtr> defInputs = matchingNodeDefs[num]->getActiveInputs();

        // Add inputs to UiNode as pins so that we can later add them to the node if necessary
        auto newNode = std::make_shared<UiNode>(node->getName(), int(++_graphTotalSize));
        newNode->setCategory(category);
        newNode->setType(type);
        newNode->setNode(node);
        newNode->_showAllInputs = true;
        node->setType(type);
        ++_graphTotalSize;
        for (mx::InputPtr input : defInputs)
        {
            UiPinPtr inPin = std::make_shared<UiPin>(_graphTotalSize, &*input->getName().begin(), input->getType(), newNode, ax::NodeEditor::PinKind::Input, input, nullptr);
            newNode->inputPins.push_back(inPin);
            _currPins.push_back(inPin);
            ++_graphTotalSize;

            if (_pinIdToLinkFrom != ed::PinId() && _pinIdToLinkTo == ed::PinId() && _menuFilterType == input->getType())
            {
                // If _pinIdToLinkFrom is a valid pin and _pinIdToLinkTo is not a valid pin,
                // it means Add Node pop-up was executed from a forward link.
                // Record the first input of the matching type as _pinIdToLinkTo.
                _pinIdToLinkTo = inPin->_pinId;
            }
        }
        std::vector<mx::OutputPtr> defOutputs = matchingNodeDefs[num]->getActiveOutputs();
        for (mx::OutputPtr output : defOutputs)
        {
            UiPinPtr outPin = std::make_shared<UiPin>(_graphTotalSize, &*output->getName().begin(), output->getType(), newNode, ax::NodeEditor::PinKind::Output, nullptr, nullptr);
            newNode->outputPins.push_back(outPin);
            _currPins.push_back(outPin);
            ++_graphTotalSize;

            if (_pinIdToLinkFrom == ed::PinId() && _pinIdToLinkTo != ed::PinId() && _menuFilterType == output->getType())
            {
                // If _pinIdToLinkFrom is not a valid pin and _pinIdToLinkTo is a valid pin,
                // it means Add Node pop-up was executed from a backward link.
                // Record the first output of the matching type as _pinIdToLinkFrom.
                _pinIdToLinkFrom = outPin->_pinId;
            }
        }

        _graphNodes.push_back(std::move(newNode));
        updateMaterials();
    }
}

int Graph::getNodeId(ed::PinId pinId)
{
    for (UiPinPtr pin : _currPins)
    {
        if (pin->_pinId == pinId)
        {
            return findNode(pin->_pinNode->getId());
        }
    }
    return -1;
}

UiPinPtr Graph::getPin(ed::PinId pinId)
{
    for (UiPinPtr pin : _currPins)
    {
        if (pin->_pinId == pinId)
        {
            return pin;
        }
    }
    UiPinPtr nullPin = std::make_shared<UiPin>(-10000, "nullPin", "null", nullptr, ax::NodeEditor::PinKind::Output, nullptr, nullptr);
    return nullPin;
}

void Graph::drawPinIcon(const std::string& type, bool connected, int alpha)
{
    ax::Drawing::IconType iconType = ax::Drawing::IconType::Flow;
    ImColor color = ImColor(0, 0, 0, 255);
    if (_pinColor.find(type) != _pinColor.end())
    {
        color = _pinColor[type];
    }

    color.Value.w = alpha / 255.0f;

    ax::Widgets::Icon(ImVec2(24, 24), iconType, connected, color, ImColor(32, 32, 32, alpha));
}

void Graph::buildGroupNode(UiNodePtr node)
{
    const float commentAlpha = 0.75f;

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, commentAlpha);
    ed::PushStyleColor(ed::StyleColor_NodeBg, ImColor(255, 255, 255, 64));
    ed::PushStyleColor(ed::StyleColor_NodeBorder, ImColor(255, 255, 255, 64));

    ed::BeginNode(node->getId());
    ImGui::PushID(node->getId());

    std::string temp = node->getMessage();
    ImVec2 messageSize = ImGui::CalcTextSize(temp.c_str());
    ImGui::PushItemWidth(messageSize.x + 15);
    ImGui::InputText("##edit", &temp);
    node->setMessage(temp);
    ImGui::PopItemWidth();
    ed::Group(ImVec2(300, 200));
    ImGui::PopID();
    ed::EndNode();
    ed::PopStyleColor(2);
    ImGui::PopStyleVar();
    if (ed::BeginGroupHint(node->getId()))
    {
        auto bgAlpha = static_cast<int>(ImGui::GetStyle().Alpha * 255);
        auto min = ed::GetGroupMin();

        ImGui::SetCursorScreenPos(min - ImVec2(-8, ImGui::GetTextLineHeightWithSpacing() + 4));
        ImGui::BeginGroup();
        ImGui::PushID(node->getId() + 1000);
        std::string tempName = node->getName();
        ImVec2 nameSize = ImGui::CalcTextSize(temp.c_str());
        ImGui::PushItemWidth(nameSize.x);
        ImGui::InputText("##edit", &tempName);
        node->setName(tempName);
        ImGui::PopID();
        ImGui::EndGroup();

        auto drawList = ed::GetHintBackgroundDrawList();

        ImRect hintBounds = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        ImRect hintFrameBounds = expandImRect(hintBounds, 8, 4);

        drawList->AddRectFilled(
            hintFrameBounds.GetTL(),
            hintFrameBounds.GetBR(),
            IM_COL32(255, 255, 255, 64 * bgAlpha / 255), 4.0f);

        drawList->AddRect(
            hintFrameBounds.GetTL(),
            hintFrameBounds.GetBR(),
            IM_COL32(0, 255, 255, 128 * bgAlpha / 255), 4.0f);
    }
    ed::EndGroupHint();
}

bool Graph::readOnly()
{
    // If the sources are not the same then the current graph cannot be modified
    return _currGraphElem->getActiveSourceUri() != _graphDoc->getActiveSourceUri();
}

void Graph::drawOutputPins(UiNodePtr node, const std::string& longestInputLabel)
{
    std::string longestLabel = longestInputLabel;
    for (UiPinPtr pin : node->outputPins)
    {
        if (pin->_name.size() > longestLabel.size())
            longestLabel = pin->_name;
    }

    // Create output pins
    float nodeWidth = ImGui::CalcTextSize(longestLabel.c_str()).x;
    for (UiPinPtr pin : node->outputPins)
    {
        const float indent = nodeWidth - ImGui::CalcTextSize(pin->_name.c_str()).x;
        ImGui::Indent(indent);
        ImGui::TextUnformatted(pin->_name.c_str());
        ImGui::SameLine();

        ed::BeginPin(pin->_pinId, ed::PinKind::Output);
        bool connected = pin->getConnected();
        if (!_pinFilterType.empty())
        {
            drawPinIcon(pin->_type, connected, _pinFilterType == pin->_type ? DEFAULT_ALPHA : FILTER_ALPHA);
        }
        else
        {
            drawPinIcon(pin->_type, connected, DEFAULT_ALPHA);
        }

        ed::EndPin();
        ImGui::Unindent(indent);
    }
}

void Graph::drawInputPin(UiPinPtr pin)
{
    ed::BeginPin(pin->_pinId, ed::PinKind::Input);
    ImGui::PushID(int(pin->_pinId.Get()));
    bool connected = pin->getConnected();
    if (!_pinFilterType.empty())
    {
        if (_pinFilterType == pin->_type)
        {
            drawPinIcon(pin->_type, connected, DEFAULT_ALPHA);
        }
        else
        {
            drawPinIcon(pin->_type, connected, FILTER_ALPHA);
        }
    }
    else
    {
        drawPinIcon(pin->_type, connected, DEFAULT_ALPHA);
    }
    ImGui::PopID();
    ed::EndPin();

    ImGui::SameLine();
    ImGui::TextUnformatted(pin->_name.c_str());
}

std::vector<int> Graph::createNodes(bool nodegraph)
{
    std::vector<int> outputNum;

    for (UiNodePtr node : _graphNodes)
    {
        if (node->getCategory() == "group")
        {
            buildGroupNode(node);
        }
        else
        {
            // Color for output pin
            std::string outputType;
            if (node->getNode() != nullptr)
            {
                ed::BeginNode(node->getId());
                ImGui::PushID(node->getId());
                ImGui::SetWindowFontScale(1.2f * _fontScale);
                ImGui::GetWindowDrawList()->AddRectFilled(
                    ImGui::GetCursorScreenPos() + ImVec2(-7.0, -8.0),
                    ImGui::GetCursorScreenPos() + ImVec2(ed::GetNodeSize(node->getId()).x - 9.f, ImGui::GetTextLineHeight() + 2.f),
                    ImColor(ImColor(55, 55, 55, 255)), 12.f);
                ImGui::GetWindowDrawList()->AddRectFilled(
                    ImGui::GetCursorScreenPos() + ImVec2(-7.0, 3),
                    ImGui::GetCursorScreenPos() + ImVec2(ed::GetNodeSize(node->getId()).x - 9.f, ImGui::GetTextLineHeight() + 2.f),
                    ImColor(ImColor(55, 55, 55, 255)), 0.f);
                ImGui::Text("%s", node->getName().c_str());
                ImGui::SetWindowFontScale(_fontScale);

                std::string longestInputLabel = node->getName();
                for (UiPinPtr pin : node->inputPins)
                {
                    UiNodePtr upUiNode = node->getConnectedNode(pin->_name);
                    if (upUiNode)
                    {
                        size_t pinIndex = 0;
                        if (upUiNode->outputPins.size() > 0)
                        {
                            const std::string outputString = pin->_input->getOutputString();
                            if (!outputString.empty())
                            {
                                for (size_t i = 0; i < upUiNode->outputPins.size(); i++)
                                {
                                    UiPinPtr outPin = upUiNode->outputPins[i];
                                    if (outPin->_name == outputString)
                                    {
                                        pinIndex = i;
                                        break;
                                    }
                                }
                            }

                            upUiNode->outputPins[pinIndex]->addConnection(pin);
                            pin->addConnection(upUiNode->outputPins[pinIndex]);
                        }
                        pin->setConnected(true);
                    }
                    if (node->_showAllInputs || (pin->getConnected() || node->getNode()->getInput(pin->_name)))
                    {
                        drawInputPin(pin);

                        if (pin->_name.size() > longestInputLabel.size())
                            longestInputLabel = pin->_name;
                    }
                }
                drawOutputPins(node, longestInputLabel);

                // Set color of output pin
                if (node->getNode()->getType() == mx::SURFACE_SHADER_TYPE_STRING)
                {
                    if (node->getOutputConnections().size() > 0)
                    {
                        for (UiNodePtr outputCon : node->getOutputConnections())
                        {
                            outputNum.push_back(findNode(outputCon->getId()));
                        }
                    }
                }
            }
            else if (node->getInput() != nullptr)
            {
                std::string longestInputLabel = node->getName();

                ed::BeginNode(node->getId());
                ImGui::PushID(node->getId());
                ImGui::SetWindowFontScale(1.2f * _fontScale);
                ImGui::GetWindowDrawList()->AddRectFilled(
                    ImGui::GetCursorScreenPos() + ImVec2(-7.0f, -8.0f),
                    ImGui::GetCursorScreenPos() + ImVec2(ed::GetNodeSize(node->getId()).x - 9.f, ImGui::GetTextLineHeight() + 2.f),
                    ImColor(ImColor(85, 85, 85, 255)), 12.f);
                ImGui::GetWindowDrawList()->AddRectFilled(
                    ImGui::GetCursorScreenPos() + ImVec2(-7.0f, 3.f),
                    ImGui::GetCursorScreenPos() + ImVec2(ed::GetNodeSize(node->getId()).x - 9.f, ImGui::GetTextLineHeight() + 2.f),
                    ImColor(ImColor(85, 85, 85, 255)), 0.f);
                ImGui::Text("%s", node->getName().c_str());
                ImGui::SetWindowFontScale(_fontScale);

                outputType = node->getInput()->getType();
                for (UiPinPtr pin : node->inputPins)
                {
                    UiNodePtr upUiNode = node->getConnectedNode(node->getName());
                    if (upUiNode)
                    {
                        if (upUiNode->outputPins.size())
                        {
                            std::string outString = pin->_output ? pin->_output->getOutputString() : mx::EMPTY_STRING;
                            size_t pinIndex = 0;
                            if (!outString.empty())
                            {
                                for (size_t i = 0; i < upUiNode->outputPins.size(); i++)
                                {
                                    if (upUiNode->outputPins[i]->_name == outString)
                                    {
                                        pinIndex = i;
                                        break;
                                    }
                                }
                            }
                            upUiNode->outputPins[pinIndex]->addConnection(pin);
                            pin->addConnection(upUiNode->outputPins[pinIndex]);
                        }
                        pin->setConnected(true);
                    }
                    ed::BeginPin(pin->_pinId, ed::PinKind::Input);
                    if (!_pinFilterType.empty())
                    {
                        if (_pinFilterType == pin->_type)
                        {
                            drawPinIcon(pin->_type, true, DEFAULT_ALPHA);
                        }
                        else
                        {
                            drawPinIcon(pin->_type, true, FILTER_ALPHA);
                        }
                    }
                    else
                    {
                        drawPinIcon(pin->_type, true, DEFAULT_ALPHA);
                    }

                    ImGui::SameLine();
                    ImGui::TextUnformatted("value");
                    ed::EndPin();

                    if (pin->_name.size() > longestInputLabel.size())
                        longestInputLabel = pin->_name;
                }
                drawOutputPins(node, longestInputLabel);
            }
            else if (node->getOutput() != nullptr)
            {
                std::string longestInputLabel = node->getName();

                ed::BeginNode(node->getId());
                ImGui::PushID(node->getId());
                ImGui::SetWindowFontScale(1.2f * _fontScale);
                ImGui::GetWindowDrawList()->AddRectFilled(
                    ImGui::GetCursorScreenPos() + ImVec2(-7.0, -8.0),
                    ImGui::GetCursorScreenPos() + ImVec2(ed::GetNodeSize(node->getId()).x - 9.f, ImGui::GetTextLineHeight() + 2.f),
                    ImColor(ImColor(35, 35, 35, 255)), 12.f);
                ImGui::GetWindowDrawList()->AddRectFilled(
                    ImGui::GetCursorScreenPos() + ImVec2(-7.0, 3),
                    ImGui::GetCursorScreenPos() + ImVec2(ed::GetNodeSize(node->getId()).x - 9.f, ImGui::GetTextLineHeight() + 2.f),
                    ImColor(ImColor(35, 35, 35, 255)), 0);
                ImGui::Text("%s", node->getName().c_str());
                ImGui::SetWindowFontScale(_fontScale);

                outputType = node->getOutput()->getType();

                for (UiPinPtr pin : node->inputPins)
                {
                    UiNodePtr upUiNode = node->getConnectedNode("");
                    if (upUiNode)
                    {
                        if (upUiNode->outputPins.size())
                        {
                            std::string outString = pin->_output ? pin->_output->getOutputString() : mx::EMPTY_STRING;
                            size_t pinIndex = 0;
                            if (!outString.empty())
                            {
                                for (size_t i = 0; i < upUiNode->outputPins.size(); i++)
                                {
                                    if (upUiNode->outputPins[i]->_name == outString)
                                    {
                                        pinIndex = i;
                                        break;
                                    }
                                }
                            }
                            upUiNode->outputPins[pinIndex]->addConnection(pin);
                            pin->addConnection(upUiNode->outputPins[pinIndex]);
                        }
                    }

                    ed::BeginPin(pin->_pinId, ed::PinKind::Input);
                    if (!_pinFilterType.empty())
                    {
                        if (_pinFilterType == pin->_type)
                        {
                            drawPinIcon(pin->_type, true, DEFAULT_ALPHA);
                        }
                        else
                        {
                            drawPinIcon(pin->_type, true, FILTER_ALPHA);
                        }
                    }
                    else
                    {
                        drawPinIcon(pin->_type, true, DEFAULT_ALPHA);
                    }
                    ImGui::SameLine();
                    ImGui::TextUnformatted("input");

                    ed::EndPin();

                    if (pin->_name.size() > longestInputLabel.size())
                        longestInputLabel = pin->_name;
                }
                drawOutputPins(node, longestInputLabel);
                if (nodegraph)
                {
                    outputNum.push_back(findNode(node->getId()));
                }
            }
            else if (node->getNodeGraph() != nullptr)
            {
                std::string longestInputLabel = node->getName();

                ed::BeginNode(node->getId());
                ImGui::PushID(node->getId());
                ImGui::SetWindowFontScale(1.2f * _fontScale);
                ImGui::GetWindowDrawList()->AddRectFilled(
                    ImGui::GetCursorScreenPos() + ImVec2(-7.0, -8.0),
                    ImGui::GetCursorScreenPos() + ImVec2(ed::GetNodeSize(node->getId()).x - 9.f, ImGui::GetTextLineHeight() + 2.f),
                    ImColor(ImColor(35, 35, 35, 255)), 12.f);
                ImGui::GetWindowDrawList()->AddRectFilled(
                    ImGui::GetCursorScreenPos() + ImVec2(-7.0, 3),
                    ImGui::GetCursorScreenPos() + ImVec2(ed::GetNodeSize(node->getId()).x - 9.f, ImGui::GetTextLineHeight() + 2.f),
                    ImColor(ImColor(35, 35, 35, 255)), 0);
                ImGui::Text("%s", node->getName().c_str());
                ImGui::SetWindowFontScale(_fontScale);
                for (UiPinPtr pin : node->inputPins)
                {
                    if (node->getConnectedNode(pin->_name) != nullptr)
                    {
                        pin->setConnected(true);
                    }
                    if (node->_showAllInputs || (pin->getConnected() || node->getNodeGraph()->getInput(pin->_name)))
                    {
                        drawInputPin(pin);

                        if (pin->_name.size() > longestInputLabel.size())
                            longestInputLabel = pin->_name;
                    }
                }
                drawOutputPins(node, longestInputLabel);
            }
            ImGui::PopID();
            ed::EndNode();
        }
    }
    ImGui::SetWindowFontScale(_fontScale);
    return outputNum;
}

void Graph::addNodeInput(UiNodePtr node, mx::InputPtr& input)
{
    if (node->getNode())
    {
        if (!node->getNode()->getInput(input->getName()))
        {
            input = node->getNode()->addInput(input->getName(), input->getType());
            input->setConnectedNode(nullptr);
        }
    }
}
void Graph::setDefaults(mx::InputPtr input)
{
    if (input->getType() == "float")
    {
        input->setValue(0.f, "float");
    }
    else if (input->getType() == "integer")
    {
        input->setValue(0, "integer");
    }
    else if (input->getType() == "color3")
    {
        input->setValue(mx::Color3(0.f, 0.f, 0.f), "color3");
    }
    else if (input->getType() == "color4")
    {
        input->setValue(mx::Color4(0.f, 0.f, 0.f, 1.f), "color4");
    }
    else if (input->getType() == "vector2")
    {
        input->setValue(mx::Vector2(0.f, 0.f), "vector2");
    }
    else if (input->getType() == "vector3")
    {
        input->setValue(mx::Vector3(0.f, 0.f, 0.f), "vector3");
    }
    else if (input->getType() == "vector4")
    {
        input->setValue(mx::Vector4(0.f, 0.f, 0.f, 0.f), "vector4");
    }
    else if (input->getType() == "string")
    {
        input->setValue("", "string");
    }
    else if (input->getType() == "filename")
    {
        input->setValue("", "filename");
    }
    else if (input->getType() == "boolean")
    {
        input->setValue(false, "boolean");
    }
}

bool Graph::checkCanAddLink(ed::PinId startPinId, ed::PinId endPinId)
{
    // Prefer to assume left to right - start is an output, end is an input; swap if inaccurate
    if (UiPinPtr endPin = getPin(endPinId); endPin && endPin->_kind != ed::PinKind::Input)
    {
        auto tmp = startPinId;
        startPinId = endPinId;
        endPinId = tmp;
    }

    ed::PinId outputPinId = startPinId;
    ed::PinId inputPinId = endPinId;
    UiPinPtr outputPin = getPin(outputPinId);
    UiPinPtr inputPin = getPin(inputPinId);

    if (!inputPin || !outputPin)
    {
        return false;
    }

    // Perform type check
    bool typesMatch = (outputPin->_type == inputPin->_type);
    if (!typesMatch)
    {
        showLabel("Invalid connection due to mismatched types", ImColor(50, 50, 50, 255));
        return false;
    }

    // Perform kind check
    bool kindsMatch = (outputPin->_kind == inputPin->_kind);
    if (kindsMatch)
    {
        showLabel("Invalid connection due to same input/output kind", ImColor(50, 50, 50, 255));
        return false;
    }

    int upNode = getNodeId(outputPinId);
    int downNode = getNodeId(inputPinId);
    UiNodePtr uiDownNode = _graphNodes[downNode];
    UiNodePtr uiUpNode = _graphNodes[upNode];
    if (!uiDownNode || !uiUpNode)
    {
        return false;
    }

    // Make sure there is an implementation for node
    const mx::ShaderGenerator& shadergen = _renderer->getGenContext().getShaderGenerator();

    // Prevent direct connecting from input to output
    if (uiDownNode->getInput() && uiUpNode->getOutput())
    {
        showLabel("Direct connections between inputs and outputs is invalid", ImColor(50, 50, 50, 255));
        return false;
    }

    // Find the implementation for this nodedef if not an input or output uinode
    if (uiDownNode->getInput() && _isNodeGraph)
    {
        showLabel("Cannot connect to inputs inside of graph", ImColor(50, 50, 50, 255));
        return false;
    }
    else if (uiUpNode->getNode())
    {
        mx::ShaderNodeImplPtr impl = shadergen.getImplementation(*_graphNodes[upNode]->getNode()->getNodeDef(), _renderer->getGenContext());
        if (!impl)
        {
            showLabel("Invalid Connection: Node does not have an implementation", ImColor(50, 50, 50, 255));
            return false;
        }
    }
    return true;
}

void Graph::addLink(ed::PinId startPinId, ed::PinId endPinId)
{
    // Prefer to assume left to right - start is an output, end is an input; swap if inaccurate
    if (UiPinPtr endPin = getPin(endPinId); endPin && endPin->_kind != ed::PinKind::Input)
    {
        auto tmp = startPinId;
        startPinId = endPinId;
        endPinId = tmp;
    }

    int end_attr = int(endPinId.Get());
    int start_attr = int(startPinId.Get());
    ed::PinId outputPinId = startPinId;
    ed::PinId inputPinId = endPinId;
    UiPinPtr outputPin = getPin(outputPinId);
    UiPinPtr inputPin = getPin(inputPinId);


    int upNode = getNodeId(outputPinId);
    int downNode = getNodeId(inputPinId);
    UiNodePtr uiDownNode = _graphNodes[downNode];
    UiNodePtr uiUpNode = _graphNodes[upNode];
    if (!uiDownNode || !uiUpNode)
    {
        return;
    }

    // If the accepting node already has a link, remove it
    if (inputPin->_connected)
    {
        for (auto iter = _currLinks.begin(); iter != _currLinks.end(); ++iter)
        {
            if (iter->_endAttr == end_attr)
            {
                // Found existing link - remove it; adapted from deleteLink
                // note: ed::BreakLinks doesn't work as the order ends up inaccurate
                deleteLinkInfo(iter->_startAttr, iter->_endAttr);
                _currLinks.erase(iter);
                break;
            }
        }
    }

    // Since we accepted new link, lets add one to our list of links.
    Link link;
    link._startAttr = start_attr;
    link._endAttr = end_attr;
    _currLinks.push_back(link);
    _frameCount = ImGui::GetFrameCount();
    _renderer->setMaterialCompilation(true);

    inputPin->addConnection(outputPin);
    outputPin->addConnection(inputPin);
    outputPin->setConnected(true);
    inputPin->setConnected(true);

    if (uiDownNode->getNode() || uiDownNode->getNodeGraph())
    {
        mx::InputPtr connectingInput = nullptr;
        for (UiPinPtr pin : uiDownNode->inputPins)
        {
            if (pin->_pinId == inputPinId)
            {
                addNodeInput(uiDownNode, pin->_input);

                // Update value to be empty
                if (uiDownNode->getNode() && uiDownNode->getNode()->getType() == mx::SURFACE_SHADER_TYPE_STRING)
                {
                    if (uiUpNode->getOutput() != nullptr)
                    {
                        pin->_input->setConnectedOutput(uiUpNode->getOutput());
                    }
                    else if (uiUpNode->getInput() != nullptr)
                    {
                        pin->_input->setConnectedInterfaceName(uiUpNode->getName());
                    }
                    else
                    {
                        if (uiUpNode->getNodeGraph() != nullptr)
                        {
                            for (UiPinPtr outPin : uiUpNode->outputPins)
                            {
                                // Set pin connection to correct output
                                if (outPin->_pinId == outputPinId)
                                {
                                    mx::OutputPtr outputs = uiUpNode->getNodeGraph()->getOutput(outPin->_name);
                                    pin->_input->setConnectedOutput(outputs);
                                }
                            }
                        }
                        else
                        {
                            mx::NodePtr upstreamNode = uiUpNode->getNode();
                            if (upstreamNode)
                            {
                                mx::OutputPtr output = upstreamNode->getOutput(outputPin->_name);
                                if (!output)
                                {
                                    output = upstreamNode->addOutput(outputPin->_name, pin->_input->getType());
                                }
                                pin->_input->setConnectedOutput(output);
                            }
                        }
                    }
                }
                else
                {
                    if (uiUpNode->getInput())
                    {
                        pin->_input->setConnectedInterfaceName(uiUpNode->getName());
                    }
                    else
                    {
                        if (uiUpNode->getNode())
                        {
                            mx::NodePtr upstreamNode = _graphNodes[upNode]->getNode();
                            mx::NodeDefPtr upstreamNodeDef = upstreamNode->getNodeDef();
                            bool isMultiOutput = upstreamNodeDef ? upstreamNodeDef->getOutputs().size() > 1 : false;
                            if (!isMultiOutput)
                            {
                                pin->_input->setConnectedNode(uiUpNode->getNode());
                            }
                            else
                            {
                                for (UiPinPtr outPin : _graphNodes[upNode]->outputPins)
                                {
                                    // Set pin connection to correct output
                                    if (outPin->_pinId == outputPinId)
                                    {
                                        mx::OutputPtr outputs = uiUpNode->getNode()->getOutput(outPin->_name);
                                        if (!outputs)
                                        {
                                            outputs = uiUpNode->getNode()->addOutput(outPin->_name, pin->_input->getType());
                                        }
                                        pin->_input->setConnectedOutput(outputs);
                                    }
                                }
                            }
                        }
                        else if (uiUpNode->getNodeGraph())
                        {
                            for (UiPinPtr outPin : uiUpNode->outputPins)
                            {
                                // Set pin connection to correct output
                                if (outPin->_pinId == outputPinId)
                                {
                                    mx::OutputPtr outputs = uiUpNode->getNodeGraph()->getOutput(outPin->_name);
                                    pin->_input->setConnectedOutput(outputs);
                                }
                            }
                        }
                    }
                }

                pin->setConnected(true);
                connectingInput = pin->_input;
                break;
            }
        }

        // Create new edge and set edge information
        createEdge(_graphNodes[upNode], _graphNodes[downNode], connectingInput);
    }
    else if (_graphNodes[downNode]->getOutput() != nullptr)
    {
        mx::InputPtr connectingInput = nullptr;
        _graphNodes[downNode]->getOutput()->setConnectedNode(_graphNodes[upNode]->getNode());

        // Create new edge and set edge information
        createEdge(_graphNodes[upNode], _graphNodes[downNode], connectingInput);
    }
    else
    {
        // Create new edge and set edge info
        UiEdge newEdge = UiEdge(_graphNodes[upNode], _graphNodes[downNode], nullptr);
        if (!edgeExists(newEdge))
        {
            _graphNodes[downNode]->edges.push_back(newEdge);
            _currEdge.push_back(newEdge);

            // Update input node num and output connections
            _graphNodes[downNode]->setInputNodeNum(1);
            _graphNodes[upNode]->setOutputConnection(_graphNodes[downNode]);
        }
    }
}

void Graph::removeEdge(int downNode, int upNode, UiPinPtr pin)
{
    int num = _graphNodes[downNode]->getEdgeIndex(_graphNodes[upNode]->getId(), pin);
    if (num != -1)
    {
        if (_graphNodes[downNode]->edges.size() == 1)
        {
            _graphNodes[downNode]->edges.erase(_graphNodes[downNode]->edges.begin() + 0);
        }
        else if (_graphNodes[downNode]->edges.size() > 1)
        {
            _graphNodes[downNode]->edges.erase(_graphNodes[downNode]->edges.begin() + num);
        }
    }

    _graphNodes[downNode]->setInputNodeNum(-1);
    _graphNodes[upNode]->removeOutputConnection(_graphNodes[downNode]->getName());
}

void Graph::deleteLinkInfo(int startAttr, int endAttr)
{
    int upNode = getNodeId(startAttr);
    int downNode = getNodeId(endAttr);
    if (upNode == -1 || downNode == -1)
    {
        return;
    }

    // Change input to default value
    if (_graphNodes[downNode]->getNode())
    {
        mx::NodeDefPtr nodeDef = _graphNodes[downNode]->getNode()->getNodeDef(_graphNodes[downNode]->getNode()->getName());

        for (UiPinPtr pin : _graphNodes[downNode]->inputPins)
        {
            if ((int) pin->_pinId.Get() == endAttr)
            {
                removeEdge(downNode, upNode, pin);
                mx::ValuePtr val = nodeDef->getActiveInput(pin->_input->getName())->getValue();
                if (_graphNodes[downNode]->getNode()->getType() == mx::SURFACE_SHADER_TYPE_STRING && _graphNodes[upNode]->getNodeGraph())
                {
                    pin->_input->setConnectedOutput(nullptr);
                }
                else
                {
                    pin->_input->setConnectedNode(nullptr);
                }
                if (_graphNodes[upNode]->getInput())
                {
                    // Remove interface value in order to set the default of the input
                    pin->_input->setConnectedInterfaceName(mx::EMPTY_STRING);
                    setDefaults(pin->_input);
                    setDefaults(_graphNodes[upNode]->getInput());
                }

                for (UiPinPtr connect : pin->_connections)
                {
                    pin->deleteConnection(connect);
                }

                // Remove any output reference
                pin->_input->removeAttribute(mx::PortElement::OUTPUT_ATTRIBUTE);
                pin->setConnected(false);

                // If a value exists update the input with it
                if (val)
                {
                    pin->_input->setValueString(val->getValueString());
                }
            }
        }
    }
    else if (_graphNodes[downNode]->getNodeGraph())
    {
        // Set default values for nodegraph node pins ie nodegraph inputs
        mx::NodeDefPtr nodeDef = _graphNodes[downNode]->getNodeGraph()->getNodeDef();
        for (UiPinPtr pin : _graphNodes[downNode]->inputPins)
        {
            if ((int) pin->_pinId.Get() == endAttr)
            {
                removeEdge(downNode, upNode, pin);
                if (_graphNodes[upNode]->getInput())
                {
                    _graphNodes[downNode]->getNodeGraph()->getInput(pin->_name)->setConnectedInterfaceName(mx::EMPTY_STRING);
                    setDefaults(_graphNodes[upNode]->getInput());
                }
                for (UiPinPtr connect : pin->_connections)
                {
                    pin->deleteConnection(connect);
                }
                pin->_input->setConnectedNode(nullptr);
                pin->setConnected(false);
                setDefaults(pin->_input);
            }
        }
    }
    else if (_graphNodes[downNode]->getOutput())
    {
        for (UiPinPtr pin : _graphNodes[downNode]->inputPins)
        {
            if ((int) pin->_pinId.Get() == endAttr)
            {
                removeEdge(downNode, upNode, pin);
                _graphNodes[downNode]->getOutput()->removeAttribute("nodename");
                for (UiPinPtr connect : pin->_connections)
                {
                    pin->deleteConnection(connect);
                }
                pin->setConnected(false);
            }
        }
    }
}

void Graph::deleteLink(ed::LinkId deletedLinkId)
{
    // If you agree that link can be deleted, accept deletion.
    if (ed::AcceptDeletedItem())
    {
        _renderer->setMaterialCompilation(true);
        _frameCount = ImGui::GetFrameCount();
        int link_id = int(deletedLinkId.Get());

        // Then remove link from your data.
        int pos = findLinkPosition(link_id);

        // Link start -1 equals node num
        Link currLink = _currLinks[pos];
        deleteLinkInfo(currLink._startAttr, currLink._endAttr);
        _currLinks.erase(_currLinks.begin() + pos);
    }
}

void Graph::deleteNode(UiNodePtr node)
{
    // Delete link
    for (UiPinPtr inputPin : node->inputPins)
    {
        UiNodePtr upNode = node->getConnectedNode(inputPin->_name);
        if (upNode)
        {
            upNode->removeOutputConnection(node->getName());
            int num = node->getEdgeIndex(upNode->getId(), inputPin);

            // Erase edge between node and up node
            if (num != -1)
            {
                if (node->edges.size() == 1)
                {
                    node->edges.erase(node->edges.begin() + 0);
                }
                else if (node->edges.size() > 1)
                {
                    node->edges.erase(node->edges.begin() + num);
                }
            }
        }
    }

    for (UiPinPtr outputPin : node->outputPins)
    {
        // Update downNode info
        for (UiPinPtr pin : outputPin.get()->getConnections())
        {
            mx::ValuePtr val;
            if (pin->_pinNode->getNode())
            {
                mx::NodeDefPtr nodeDef = pin->_pinNode->getNode()->getNodeDef(pin->_pinNode->getNode()->getName());
                val = nodeDef->getActiveInput(pin->_input->getName())->getValue();
                if (pin->_pinNode->getNode()->getType() == mx::SURFACE_SHADER_TYPE_STRING)
                {
                    pin->_input->setConnectedOutput(nullptr);
                }
                else
                {
                    pin->_input->setConnectedNode(nullptr);
                }
                if (node->getInput())
                {
                    // Remove interface in order to set the default of the input
                    pin->_input->setConnectedInterfaceName(mx::EMPTY_STRING);
                    setDefaults(pin->_input);
                    setDefaults(node->getInput());
                }
            }
            else if (pin->_pinNode->getNodeGraph())
            {
                if (node->getInput())
                {
                    pin->_pinNode->getNodeGraph()->getInput(pin->_name)->setConnectedInterfaceName(mx::EMPTY_STRING);
                    setDefaults(node->getInput());
                }
                pin->_input->setConnectedNode(nullptr);
                pin->setConnected(false);
                setDefaults(pin->_input);
            }

            pin->setConnected(false);
            if (val)
            {
                pin->_input->setValueString(val->getValueString());
            }

            int num = pin->_pinNode->getEdgeIndex(node->getId(), pin);
            if (num != -1)
            {
                if (pin->_pinNode->edges.size() == 1)
                {
                    pin->_pinNode->edges.erase(pin->_pinNode->edges.begin() + 0);
                }
                else if (pin->_pinNode->edges.size() > 1)
                {
                    pin->_pinNode->edges.erase(pin->_pinNode->edges.begin() + num);
                }
            }

            pin->_pinNode->setInputNodeNum(-1);

            // Not really necessary since it will be deleted
            node->removeOutputConnection(pin->_pinNode->getName());
        }
    }

    // Remove from NodeGraph
    // All link information is handled in delete link which is called before this
    int nodeNum = findNode(node->getId());
    _currGraphElem->removeChild(node->getName());
    _graphNodes.erase(_graphNodes.begin() + nodeNum);
}

void Graph::addNodeGraphPins()
{
    for (UiNodePtr node : _graphNodes)
    {
        if (node->getNodeGraph())
        {
            if (node->inputPins.size() != node->getNodeGraph()->getInputs().size())
            {
                for (mx::InputPtr input : node->getNodeGraph()->getInputs())
                {
                    std::string name = input->getName();
                    auto result = std::find_if(node->inputPins.begin(), node->inputPins.end(), [name](UiPinPtr x)
                    {
                        return x->_name == name;
                    });
                    if (result == node->inputPins.end())
                    {
                        UiPinPtr inPin = std::make_shared<UiPin>(++_graphTotalSize, &*input->getName().begin(), input->getType(), node, ax::NodeEditor::PinKind::Input, input, nullptr);
                        node->inputPins.push_back(inPin);
                        _currPins.push_back(inPin);
                        ++_graphTotalSize;
                    }
                }
            }
            if (node->outputPins.size() != node->getNodeGraph()->getOutputs().size())
            {
                for (mx::OutputPtr output : node->getNodeGraph()->getOutputs())
                {
                    std::string name = output->getName();
                    auto result = std::find_if(node->outputPins.begin(), node->outputPins.end(), [name](UiPinPtr x)
                    {
                        return x->_name == name;
                    });
                    if (result == node->outputPins.end())
                    {
                        UiPinPtr outPin = std::make_shared<UiPin>(++_graphTotalSize, &*output->getName().begin(), output->getType(), node, ax::NodeEditor::PinKind::Output, nullptr, nullptr);
                        ++_graphTotalSize;
                        node->outputPins.push_back(outPin);
                        _currPins.push_back(outPin);
                    }
                }
            }
        }
    }
}

void Graph::upNodeGraph()
{
    if (!_graphStack.empty())
    {
        savePosition();
        _graphNodes = _graphStack.top();
        _currPins = _pinStack.top();
        _graphTotalSize = _sizeStack.top();
        addNodeGraphPins();
        _graphStack.pop();
        _pinStack.pop();
        _sizeStack.pop();
        _currGraphName.pop_back();
        _initial = true;
        ed::NavigateToContent();
        if (_currUiNode)
        {
            ed::DeselectNode(_currUiNode->getId());
            _currUiNode = nullptr;
        }
        _prevUiNode = nullptr;
        _isNodeGraph = false;
        _currGraphElem = _graphDoc;
        _initial = true;
    }
}

void Graph::clearGraph()
{
    _graphNodes.clear();
    _currLinks.clear();
    _currEdge.clear();
    _newLinks.clear();
    _currPins.clear();
    _graphDoc = mx::createDocument();
    _graphDoc->setDataLibrary(_stdLib);
    _currGraphElem = _graphDoc;

    if (_currUiNode != nullptr)
    {
        ed::DeselectNode(_currUiNode->getId());
        _currUiNode = nullptr;
    }
    _prevUiNode = nullptr;
    _currRenderNode = nullptr;
    _isNodeGraph = false;
    _currGraphName.clear();

    _renderer->setDocument(_graphDoc);
    _renderer->updateMaterials(nullptr);
}

void Graph::loadGraphFromFile(bool prompt)
{
    // Deselect node before loading new file
    if (_currUiNode)
    {
        ed::DeselectNode(_currUiNode->getId());
        _currUiNode = nullptr;
    }

    if (prompt || _materialFilename.isEmpty())
    {
        _fileDialog.setTitle("Open File");
        _fileDialog.setTypeFilters(_mtlxFilter);
        _fileDialog.open();
    }
    else
    {
        _graphDoc = loadDocument(_materialFilename);

        // Rebuild the UI
        _initial = true;
        buildUiBaseGraph(_graphDoc);
        _currGraphElem = _graphDoc;
        _prevUiNode = nullptr;

        _renderer->setDocument(_graphDoc);
        _renderer->updateMaterials(nullptr);
    }
}

void Graph::saveGraphToFile()
{
    _fileDialogSave.setTypeFilters(_mtlxFilter);
    _fileDialogSave.setTitle("Save File As");
    _fileDialogSave.open();
}

void Graph::loadGeometry()
{
    _fileDialogGeom.setTitle("Load Geometry");
    _fileDialogGeom.setTypeFilters(_geomFilter);
    _fileDialogGeom.open();
}

void Graph::graphButtons()
{
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.15f, .15f, .15f, 1.0f));
    ImGui::SetWindowFontScale(_fontScale);

    if (ImGui::BeginMenuBar())
    {
        if (ImGui::BeginMenu("File"))
        {
            // Buttons for loading and saving a .mtlx
            if (ImGui::MenuItem("New", "Ctrl-N"))
            {
                clearGraph();
            }
            else if (ImGui::MenuItem("Open", "Ctrl-O"))
            {
                loadGraphFromFile(true);
            }
            else if (ImGui::MenuItem("Reload", "Ctrl-R"))
            {
                loadGraphFromFile(false);
            }
            else if (ImGui::MenuItem("Save", "Ctrl-S"))
            {
                saveGraphToFile();
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Graph"))
        {
            if (ImGui::MenuItem("Auto Layout"))
            {
                _autoLayout = true;
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Viewer"))
        {
            if (ImGui::MenuItem("Load Geometry"))
            {
                loadGeometry();
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Options"))
        {
            ImGui::Checkbox("Save Node Positions", &_saveNodePositions);
            ImGui::EndMenu();
        }

        if (ImGui::Button("Help"))
        {
            ImGui::OpenPopup("Help");
        }
        if (ImGui::BeginPopup("Help"))
        {
            showHelp();
            ImGui::EndPopup();
        }

        ImGui::EndMenuBar();
    }

    // Menu keys
    ImGuiIO& guiIO = ImGui::GetIO();
    if (guiIO.KeyCtrl && !_fileDialogSave.isOpened() && !_fileDialog.isOpened() && !_fileDialogGeom.isOpened())
    {
        if (ImGui::IsKeyReleased(ImGuiKey_O))
        {
            loadGraphFromFile(true);
        }
        else if (ImGui::IsKeyReleased(ImGuiKey_N))
        {
            clearGraph();
        }
        else if (ImGui::IsKeyReleased(ImGuiKey_R))
        {
            loadGraphFromFile(false);
        }
        else if (ImGui::IsKeyReleased(ImGuiKey_S))
        {
            saveGraphToFile();
        }
    }

    // Split window into panes for NodeEditor
    static float leftPaneWidth = 375.0f;
    static float rightPaneWidth = 750.0f;
    splitter(true, 4.0f, &leftPaneWidth, &rightPaneWidth, 20.0f, 20.0f);

    // Create back button and graph hierarchy name display
    ImGui::Indent(leftPaneWidth + 15.f);
    if (ImGui::Button("<"))
    {
        upNodeGraph();
    }
    ImGui::SameLine();
    if (!_currGraphName.empty())
    {
        for (std::string name : _currGraphName)
        {
            ImGui::Text("%s", name.c_str());
            ImGui::SameLine();
            if (name != _currGraphName.back())
            {
                ImGui::Text(">");
                ImGui::SameLine();
            }
        }
    }
    ImVec2 windowPos2 = ImGui::GetWindowPos();
    ImGui::Unindent(leftPaneWidth + 15.f);
    ImGui::PopStyleColor();
    ImGui::NewLine();

    // Create two windows using splitter
    float paneWidth = (leftPaneWidth - 2.0f);

    float aspectRatio = _renderer->getPixelRatio();
    ImVec2 screenSize = ImVec2(paneWidth, paneWidth / aspectRatio);

    ImVec2 mousePos = ImGui::GetMousePos();
    ImVec2 tempWindowPos = ImGui::GetCursorPos();
    bool cursorInRenderView = mousePos.x > tempWindowPos.x && mousePos.x < (tempWindowPos.x + screenSize.x) &&
                              mousePos.y > tempWindowPos.y && mousePos.y < (tempWindowPos.y + screenSize.y);

    ImGuiWindowFlags windowFlags = 0;

    if (cursorInRenderView)
    {
        windowFlags |= ImGuiWindowFlags_NoScrollWithMouse;
    }

    ImGui::BeginChild("Selection", ImVec2(paneWidth, 0), false, windowFlags);
    ImVec2 windowPos = ImGui::GetWindowPos();

    // Update cursorInRenderView to account for other windows overlapping the Render View (e.g. Menu dropdown).
    cursorInRenderView &= ImGui::IsWindowHovered(ImGuiHoveredFlags_None);

    // Update cursorInRenderView to account for visible scrollbar and scroll amount.
    ImGuiContext* context = ImGui::GetCurrentContext();
    bool hasScrollbar = context->CurrentWindow->ScrollbarY;
    cursorInRenderView &= hasScrollbar ? mousePos.x < (tempWindowPos.x + screenSize.x - ImGui::GetStyle().ScrollbarSize) : true;
    cursorInRenderView &= hasScrollbar ? mousePos.y < (tempWindowPos.y + screenSize.y - ImGui::GetScrollY()) : true;

    // RenderView window
    ImVec2 wsize = ImVec2((float) _renderer->getViewWidth(), (float) _renderer->getViewHeight());
    _renderer->setViewWidth((int) screenSize[0]);
    _renderer->setViewHeight((int) screenSize[1]);

    if (_renderer)
    {
        // Enable sRGB conversion for framebuffer ONLY when drawing material preview
        ImGui::GetWindowDrawList()->AddCallback(EnableSRGBCallback,  nullptr);

        _renderer->getViewCamera()->setViewportSize(mx::Vector2(screenSize[0], screenSize[1]));
        GLuint64 my_image_texture = _renderer->_textureID;
        mx::Vector2 vec = _renderer->getViewCamera()->getViewportSize();

        ImGui::Image((ImTextureID) my_image_texture, screenSize, ImVec2(0, 1), ImVec2(1, 0));

        // Disable sRGB conversion for all other imgui ui components.
        ImGui::GetWindowDrawList()->AddCallback(DisableSRGBCallback,  nullptr);
    }

    ImGui::Separator();

    // Property editor for current nodes
    propertyEditor();
    ImGui::EndChild();
    ImGui::SameLine(0.0f, 12.0f);

    if (cursorInRenderView)
    {
        handleRenderViewInputs();
    }
}

void Graph::propertyEditor()
{
    // Get parent dimensions
    ImVec2 textPos = ImGui::GetCursorScreenPos(); // Position for the background
    float parentWidth = ImGui::GetContentRegionAvail().x; // Available width in the parent
    
    // Draw the title bar
    const ImGuiStyle& style = ImGui::GetStyle();
    ImVec4 menuBarBgColor = style.Colors[ImGuiCol_MenuBarBg]; 
    ImU32 bgColor = ImGui::ColorConvertFloat4ToU32(menuBarBgColor); // Convert to 32-bit color
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(textPos,
                            ImVec2(textPos.x + parentWidth, textPos.y + ImGui::GetTextLineHeight()),
                            bgColor);
    ImGui::Text("Node Property Editor");

    if (_currUiNode)
    {
        // Set and edit name
        ImGui::Text("Name: ");
        ImGui::SameLine();
        std::string original = _currUiNode->getName();
        std::string temp = original;
        float availableWidth = ImGui::GetContentRegionAvail().x; 
        ImGui::PushItemWidth(availableWidth); 
        ImGui::InputText("##edit", &temp);
        ImGui::PopItemWidth(); 

        std::string docString = "NodeDef Doc String: \n";
        if (_currUiNode->getNode())
        {
            if (temp != original)
            {
                std::string name = _currUiNode->getNode()->getParent()->createValidChildName(temp);

                std::vector<UiNodePtr> downstreamNodes = _currUiNode->getOutputConnections();
                for (UiNodePtr uiNode : downstreamNodes)
                {
                    if (!uiNode->getInput() && uiNode->getNode())
                    {
                        for (mx::InputPtr input : uiNode->getNode()->getActiveInputs())
                        {
                            if (input->getConnectedNode() == _currUiNode->getNode())
                            {
                                _currUiNode->getNode()->setName(name);
                                uiNode->getNode()->setConnectedNode(input->getName(), _currUiNode->getNode());
                            }
                        }
                    }
                }
                _currUiNode->setName(name);
                _currUiNode->getNode()->setName(name);
            }
        }
        else if (_currUiNode->getInput())
        {
            if (temp != original)
            {
                std::string name = _currUiNode->getInput()->getParent()->createValidChildName(temp);
                std::vector<UiNodePtr> downstreamNodes = _currUiNode->getOutputConnections();
                for (UiNodePtr uiNode : downstreamNodes)
                {
                    if (uiNode->getInput() == nullptr)
                    {
                        if (uiNode->getNode())
                        {
                            for (mx::InputPtr input : uiNode->getNode()->getActiveInputs())
                            {
                                if (input->getInterfaceInput() == _currUiNode->getInput())
                                {
                                    _currUiNode->getInput()->setName(name);
                                    mx::ValuePtr val = _currUiNode->getInput()->getValue();
                                    input->setConnectedInterfaceName(name);
                                    mx::InputPtr pt = input->getInterfaceInput();
                                }
                            }
                        }
                        else
                        {
                            uiNode->getOutput()->setConnectedNode(_currUiNode->getNode());
                        }
                    }
                }

                _currUiNode->getInput()->setName(name);
                _currUiNode->setName(name);
            }
        }
        else if (_currUiNode->getOutput())
        {
            if (temp != original)
            {
                std::string name = _currUiNode->getOutput()->getParent()->createValidChildName(temp);
                _currUiNode->getOutput()->setName(name);
                _currUiNode->setName(name);
            }
        }
        else if (_currUiNode->getCategory() == "group")
        {
            _currUiNode->setName(temp);
        }
        else if (_currUiNode->getCategory() == "nodegraph")
        {
            if (temp != original)
            {
                std::string name = _currUiNode->getNodeGraph()->getParent()->createValidChildName(temp);
                _currUiNode->getNodeGraph()->setName(name);
                _currUiNode->setName(name);

                for (UiNodePtr node : _graphNodes)
                {
                    if (!node->getInput())
                    {
                        std::vector<UiPinPtr> inputs = node->inputPins;
                        for (size_t i = 0; i < inputs.size(); i++)
                        {
                            const std::string& inputName = inputs[i]->_name;
                            UiNodePtr inputNode = node->getConnectedNode(inputName);
                            if (inputNode && inputNode->getName() == name && node->getNode())
                            {
                                node->getNode()->getInput(inputName)->setAttribute("nodegraph", name);
                            }
                        }
                    }
                }
            }
        }

        const float TEXT_BASE_HEIGHT = ImGui::GetTextLineHeightWithSpacing() * 1.3f;
        const int SCROLL_LINE_COUNT = 20;
        ImGuiTableFlags tableFlags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings |
                                     ImGuiTableFlags_BordersOuterH | ImGuiTableFlags_NoBordersInBody;

        ImGui::Text("Category:");
        ImGui::SameLine();

        // Change button color to match background
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.096f, .096f, .096f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.1f, .1f, .1f, 1.0f));
        if (_currUiNode->getNode())
        {
            ImGui::NextColumn();
            ImGui::Text("%s", _currUiNode->getNode()->getCategory().c_str());
            docString += _currUiNode->getNode()->getCategory();
            if (_currUiNode->getNode()->getNodeDef())
            {
                docString += ":";
                docString += _currUiNode->getNode()->getNodeDef()->getDocString() + "\n";
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                ImGui::SetTooltip("%s", _currUiNode->getNode()->getNodeDef()->getDocString().c_str());
            }

            ImGui::Checkbox("Show all inputs", &_currUiNode->_showAllInputs);

            int count = 0;
            for (UiPinPtr input : _currUiNode->inputPins)
            {
                if (_currUiNode->_showAllInputs || (input->getConnected() || _currUiNode->getNode()->getInput(input->_name)))
                {
                    count++;
                }
            }
            if (count)
            {
                ImVec2 tableSize(0.0f, TEXT_BASE_HEIGHT * std::min(SCROLL_LINE_COUNT, count));
                bool haveTable = ImGui::BeginTable("inputs_node_table", 2, tableFlags, tableSize);
                if (haveTable)
                {
                    ImGui::SetWindowFontScale(_fontScale);
                    for (UiPinPtr input : _currUiNode->inputPins)
                    {
                        if (_currUiNode->_showAllInputs || (input->getConnected() || _currUiNode->getNode()->getInput(input->_name)))
                        {
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();

                            mx::UIProperties uiProperties;
                            mx::getUIProperties(input->_input, mx::EMPTY_STRING, uiProperties);
                            std::string inputLabel = !uiProperties.uiName.empty() ? uiProperties.uiName : input->_input->getName();
                            mx::OutputPtr out = input->_input->getConnectedOutput();

                            // Set comment help box
                            ImGui::PushID(int(input->_pinId.Get()));
                            ImGui::Text("%s", inputLabel.c_str());
                            mx::InputPtr tempInt = _currUiNode->getNode()->getNodeDef()->getActiveInput(input->_input->getName());
                            docString += input->_name;
                            docString += ": ";
                            if (tempInt)
                            {
                                std::string newStr = _currUiNode->getNode()->getNodeDef()->getActiveInput(input->_input->getName())->getDocString();
                                if (newStr != mx::EMPTY_STRING)
                                {
                                    docString += newStr;
                                }
                            }
                            docString += "\t \n";

                            // Set constant sliders for input values
                            ImGui::TableNextColumn();
                            if (!input->getConnected())
                            {
                                setConstant(_currUiNode, input->_input, uiProperties);
                            }
                            else
                            {
                                std::string typeText = " [" + input->_input->getType() + "]";
                                ImGui::Text("%s", typeText.c_str());
                            }

                            ImGui::PopID();
                        }
                    }

                    ImGui::EndTable();
                    ImGui::SetWindowFontScale(1.0f);
                }
            }
        }

        else if (_currUiNode->getInput() != nullptr)
        {
            ImGui::Text("%s", _currUiNode->getCategory().c_str());
            std::vector<UiPinPtr> inputs = _currUiNode->inputPins;

            int count = static_cast<int>(inputs.size());
            if (count)
            {
                bool haveTable = ImGui::BeginTable("inputs_input_table", 2, tableFlags,
                                                   ImVec2(0.0f, TEXT_BASE_HEIGHT * std::min(SCROLL_LINE_COUNT, count)));
                if (haveTable)
                {
                    ImGui::SetWindowFontScale(_fontScale);
                    for (size_t i = 0; i < inputs.size(); i++)
                    {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();

                        mx::InputPtr mxinput = inputs[i]->_input;
                        mx::UIProperties uiProperties;
                        mx::getUIProperties(mxinput, mx::EMPTY_STRING, uiProperties);
                        std::string inputLabel = !uiProperties.uiName.empty() ? uiProperties.uiName : mxinput->getName();

                        // Set comment help box
                        ImGui::PushID(int(inputs[i]->_pinId.Get()));
                        ImGui::Text("%s", inputLabel.c_str());

                        ImGui::TableNextColumn();

                        // Set constant sliders for input values
                        if (!inputs[i]->getConnected())
                        {
                            setConstant(_currUiNode, inputs[i]->_input, uiProperties);
                        }
                        else
                        {
                            std::string typeText = " [" + inputs[i]->_input->getType() + "]";
                            ImGui::Text("%s", typeText.c_str());
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                    ImGui::SetWindowFontScale(1.0f);
                }
            }
        }
        else if (_currUiNode->getOutput() != nullptr)
        {
            ImGui::Text("%s", _currUiNode->getOutput()->getCategory().c_str());
        }
        else if (_currUiNode->getNodeGraph() != nullptr)
        {
            std::vector<UiPinPtr> inputs = _currUiNode->inputPins;
            ImGui::Text("%s", _currUiNode->getCategory().c_str());
            int count = 0;
            for (UiPinPtr input : inputs)
            {
                if (_currUiNode->_showAllInputs || (input->getConnected() || _currUiNode->getNodeGraph()->getInput(input->_name)))
                {
                    count++;
                }
            }
            if (count)
            {
                bool haveTable = ImGui::BeginTable("inputs_nodegraph_table", 2, tableFlags,
                                                   ImVec2(0.0f, TEXT_BASE_HEIGHT * std::min(SCROLL_LINE_COUNT, count)));
                if (haveTable)
                {
                    ImGui::SetWindowFontScale(_fontScale);
                    for (UiPinPtr input : inputs)
                    {
                        if (_currUiNode->_showAllInputs || (input->getConnected() || _currUiNode->getNodeGraph()->getInput(input->_name)))
                        {
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();

                            mx::InputPtr mxinput = input->_input;
                            mx::UIProperties uiProperties;
                            mx::getUIProperties(mxinput, mx::EMPTY_STRING, uiProperties);
                            std::string inputLabel = !uiProperties.uiName.empty() ? uiProperties.uiName : mxinput->getName();

                            // Set comment help box
                            ImGui::PushID(int(input->_pinId.Get()));
                            ImGui::Text("%s", inputLabel.c_str());

                            docString += _currUiNode->getNodeGraph()->getActiveInput(input->_input->getName())->getDocString();

                            ImGui::TableNextColumn();
                            if (!input->_input->getConnectedNode() && _currUiNode->getNodeGraph()->getActiveInput(input->_input->getName()))
                            {
                                setConstant(_currUiNode, input->_input, uiProperties);
                            }
                            else
                            {
                                std::string typeText = " [" + input->_input->getType() + "]";
                                ImGui::Text("%s", typeText.c_str());
                            }

                            ImGui::PopID();
                        }
                    }
                    ImGui::EndTable();
                    ImGui::SetWindowFontScale(1.0f);
                }
            }
            ImGui::Checkbox("Show all inputs", &_currUiNode->_showAllInputs);
        }
        ImGui::PopStyleColor();
        ImGui::PopStyleColor();

        if (ImGui::Button("Node Info"))
        {
            ImGui::OpenPopup("docstring");
        }

        if (ImGui::BeginPopup("docstring"))
        {
            ImGui::SetWindowFontScale(_fontScale);
            ImGui::Text("%s", docString.c_str());
            ImGui::SetWindowFontScale(1.0f);
            ImGui::EndPopup();
        }
    }
}

void Graph::showHelp() const
{
    ImGui::Text("MATERIALX GRAPH EDITOR HELP");
    if (ImGui::CollapsingHeader("Graph"))
    {
        if (ImGui::TreeNode("Navigation"))
        {
            ImGui::BulletText("F : Frame selected nodes in graph.");
            ImGui::BulletText("RIGHT MOUSE button to pan.");
            ImGui::BulletText("SCROLL WHEEL to zoom.");
            ImGui::BulletText("\"<\" BUTTON to view parent of current graph");
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Editing"))
        {
            ImGui::BulletText("TAB : Show popup menu to add new nodes.");
            ImGui::BulletText("CTRL-C : Copy selected nodes to clipboard.");
            ImGui::BulletText("CTRL-V : Paste clipboard to graph.");
            ImGui::BulletText("CTRL-F : Find a node by name.");
            ImGui::BulletText("CTRL-X : Delete selected nodes and add to clipboard.");
            ImGui::BulletText("DELETE : Delete selected nodes or connections.");
            ImGui::TreePop();
        }
    }
    if (ImGui::CollapsingHeader("Viewer"))
    {
        ImGui::BulletText("LEFT MOUSE button to tumble.");
        ImGui::BulletText("RIGHT MOUSE button to pan.");
        ImGui::BulletText("SCROLL WHEEL to zoom.");
        ImGui::BulletText("Keypad +/- to zoom in fixed increments");
    }

    if (ImGui::CollapsingHeader("Property Editor"))
    {
        ImGui::BulletText("UP/DOWN ARROW to move between inputs.");
        ImGui::BulletText("LEFT-MOUSE DRAG to modify values while entry field is in focus.");
        ImGui::BulletText("DBL_CLICK or CTRL+CLICK LEFT-MOUSE on entry field to input values.");
        ImGui::Separator();
        ImGui::BulletText("\"Show all inputs\" Will toggle between showing all inputs and\n only those that have been modified.");
        ImGui::BulletText("\"Node Info\" Will toggle showing node information.");
    }
}

void Graph::addNodePopup(bool cursor)
{
    bool open_AddPopup = (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootWindow) && ImGui::IsKeyReleased(ImGuiKey_Tab)) ||
        (_pinFilterType != mx::EMPTY_STRING && ImGui::IsMouseReleased(0));
    static char input[32]{ "" };
    if (open_AddPopup)
    {
        cursor = true;
        ImGui::OpenPopup("add node");
        _menuFilterType = _pinFilterType;
    }
    if (ImGui::BeginPopup("add node"))
    {
        ImGui::Text("Add Node");
        ImGui::Separator();
        if (cursor)
        {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::InputText("##input", input, sizeof(input));
        std::string subs(input);

        // Input string length
        // Filter extra nodes - includes inputs, outputs, groups, and node graphs
        const std::string NODEGRAPH_ENTRY = "Node Graph";

        // Filter nodedefs and add to menu if matches filter
        for (auto node : _nodesToAdd)
        {
            if (_menuFilterType != mx::EMPTY_STRING)
            {
                // _menuFilterType is not empty, so the Add Node pop-up was triggered by drawing a Link.
                if (_pinIdToLinkFrom != ed::PinId() && _pinIdToLinkTo == ed::PinId())
                {
                    // Drawing a forward Link from an output pin
                    // Filter out nodes that has no inputs of the type matching to the _menuFilterType
                    if (node.getInputTypes().count(_menuFilterType)==0)
                    {
                        continue;
                    }
                }
                else if (_pinIdToLinkFrom == ed::PinId() && _pinIdToLinkTo != ed::PinId())
                {
                    // Drawing a backward Link from an input pin
                    // Filter out nodes whose type do not match the _menuFilterType
                    if (node.getOutputTypes().count(_menuFilterType)==0)
                    {
                        continue;
                    }
                }
            }

            // Filter out list of nodes
            if (subs.size() > 0)
            {
                ImGui::SetNextWindowSizeConstraints(ImVec2(250.0f, 300.0f), ImVec2(-1.0f, 500.0f));
                std::string str(node.getName());
                std::string nodeName = node.getName();

                // Disallow creating nested nodegraphs
                if (_isNodeGraph && node.getGroup() == NODEGRAPH_ENTRY)
                {
                    continue;
                }

                // Allow spaces to be used to search for node names
                std::replace(subs.begin(), subs.end(), ' ', '_');

                if (subs.size() == 0 || str.find(subs) != std::string::npos)
                {
                    if (ImGui::MenuItem(getUserNodeDefName(nodeName).c_str()) || (ImGui::IsItemFocused() && ImGui::IsKeyPressedMap(ImGuiKey_Enter)))
                    {
                        addNode(node.getCategory(), getUserNodeDefName(nodeName), node.getType());
                        _addNewNode = true;
                        memset(input, '\0', sizeof(input));
                    }
                }
            }
            else
            {
                ImGui::SetNextWindowSizeConstraints(ImVec2(100, 10), ImVec2(-1, 300));
                if (ImGui::BeginMenu(node.getGroup().c_str()))
                {
                    ImGui::SetWindowFontScale(_fontScale);
                    std::string name = node.getName();
                    std::string prefix = "ND_";
                    if (name.compare(0, prefix.size(), prefix) == 0 && name.compare(prefix.size(), std::string::npos, node.getCategory()) == 0)
                    {
                        if (ImGui::MenuItem(getUserNodeDefName(name).c_str()) || (ImGui::IsItemFocused() && ImGui::IsKeyPressedMap(ImGuiKey_Enter)))
                        {
                            addNode(node.getCategory(), getUserNodeDefName(name), node.getType());
                            _addNewNode = true;
                        }
                    }
                    else
                    {
                        if (ImGui::BeginMenu(node.getCategory().c_str()))
                        {
                            if (ImGui::MenuItem(getUserNodeDefName(name).c_str()) || (ImGui::IsItemFocused() && ImGui::IsKeyPressedMap(ImGuiKey_Enter)))
                            {
                                addNode(node.getCategory(), getUserNodeDefName(name), node.getType());
                                _addNewNode = true;
                            }
                            ImGui::EndMenu();
                        }
                    }

                    ImGui::EndMenu();
                }
            }
        }
        ImGui::EndPopup();
        open_AddPopup = false;
    }
}

void Graph::searchNodePopup(bool cursor)
{
    const bool open_search = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyDown(ImGuiKey_F) && ImGui::IsKeyDown(ImGuiKey_LeftCtrl);
    if (open_search)
    {
        cursor = true;
        ImGui::OpenPopup("search");
    }
    if (ImGui::BeginPopup("search"))
    {
        ed::NavigateToSelection();
        static ImGuiTextFilter filter;
        ImGui::Text("Search for Node:");
        static char input[16]{ "" };
        ImGui::SameLine();
        if (cursor)
        {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::InputText("##input", input, sizeof(input));

        if (std::string(input).size() > 0)
        {
            for (UiNodePtr node : _graphNodes)
            {
                if (node->getName().find(std::string(input)) != std::string::npos)
                {

                    if (ImGui::MenuItem(node->getName().c_str()) || (ImGui::IsItemFocused() && ImGui::IsKeyPressedMap(ImGuiKey_Enter)))
                    {
                        _searchNodeId = node->getId();
                        memset(input, '\0', sizeof(input));
                    }
                }
            }
        }
        ImGui::EndPopup();
    }
}

bool Graph::isPinHovered()
{
    ed::PinId currentPin = ed::GetHoveredPin();
    ed::PinId nullPin = 0;
    return currentPin != nullPin;
}

void Graph::addPinPopup()
{
    // Add a floating popup to pin when hovered
    if (isPinHovered())
    {
        ed::Suspend();
        UiPinPtr pin = getPin(ed::GetHoveredPin());
        std::string connected;
        std::string value;
        if (pin->_connected)
        {
            mx::StringVec connectedNames;
            for (UiPinPtr connectedPin : pin->getConnections())
            {
                connectedNames.push_back(connectedPin->_name);
            }
            connected = "\nConnected to " + mx::joinStrings(connectedNames, ", ");
        }
        else if (pin->_input)
        {
            value = "\nValue: " + pin->_input->getValueString();
        }
        const std::string message("Name: " + pin->_name + "\nType: " + pin->_type + value + connected);
        ImGui::SetTooltip("%s", message.c_str());
        ed::Resume();
    }
}

void Graph::readOnlyPopup()
{
    if (_popup)
    {
        ImGui::SetNextWindowSize(ImVec2(200, 100));
        ImGui::OpenPopup("Read Only");
        _popup = false;
    }
    if (ImGui::BeginPopup("Read Only"))
    {
        ImGui::Text("This graph is Read Only");
        ImGui::EndPopup();
    }
}

void Graph::shaderPopup()
{
    if (_renderer->getMaterialCompilation())
    {
        ImGui::SetNextWindowPos(ImVec2((float) _renderer->getViewWidth() - 135, (float) _renderer->getViewHeight() + 5));
        ImGui::SetNextWindowBgAlpha(80.f);
        ImGui::OpenPopup("Shaders");
    }
    if (ImGui::BeginPopup("Shaders"))
    {
        ImGui::Text("Compiling Shaders");
        if (!_renderer->getMaterialCompilation())
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void Graph::handleRenderViewInputs()
{
    ImVec2 mousePos = ImGui::GetMousePos();
    mx::Vector2 mxMousePos = mx::Vector2(mousePos.x, mousePos.y);
    float scrollAmt = ImGui::GetIO().MouseWheel;
    int button = -1;
    bool down = false;
    if (ImGui::IsMouseDragging(0) || ImGui::IsMouseDragging(1))
    {
        _renderer->setMouseMotionEvent(mxMousePos);
    }
    if (ImGui::IsMouseClicked(0))
    {
        button = 0;
        down = true;
        _renderer->setMouseButtonEvent(button, down, mxMousePos);
    }
    else if (ImGui::IsMouseClicked(1))
    {
        button = 1;
        down = true;
        _renderer->setMouseButtonEvent(button, down, mxMousePos);
    }
    else if (ImGui::IsMouseReleased(0))
    {
        button = 0;
        _renderer->setMouseButtonEvent(button, down, mxMousePos);
    }
    else if (ImGui::IsMouseReleased(1))
    {
        button = 1;
        _renderer->setMouseButtonEvent(button, down, mxMousePos);
    }
    else if (ImGui::IsKeyPressed(ImGuiKey_KeypadAdd))
    {
        _renderer->setKeyEvent(ImGuiKey_KeypadAdd);
    }
    else if (ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract))
    {
        _renderer->setKeyEvent(ImGuiKey_KeypadSubtract);
    }

    // Scrolling not possible if open or save file dialog is open
    if (scrollAmt != 0 && !_fileDialogSave.isOpened() && !_fileDialog.isOpened() && !_fileDialogGeom.isOpened())
    {
        _renderer->setScrollEvent(scrollAmt);
    }
}

void Graph::drawGraph(ImVec2 mousePos)
{
    if (_searchNodeId > 0)
    {
        ed::SelectNode(_searchNodeId);
        ed::NavigateToSelection();
        _searchNodeId = -1;
    }

    bool TextCursor = false;

    // Center imgui window and set size
    ImGuiIO& io2 = ImGui::GetIO();
    ImGui::SetNextWindowSize(io2.DisplaySize);
    ImGui::SetNextWindowPos(ImVec2(io2.DisplaySize.x * 0.5f, io2.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::Begin("MaterialX", nullptr, ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings);

    io2.ConfigFlags = ImGuiConfigFlags_IsSRGB | ImGuiConfigFlags_NavEnableKeyboard;
    io2.MouseDoubleClickTime = .5;
    graphButtons();

    ed::Begin("My Editor");
    {
        ed::Suspend();

        // Set up popups for adding a node when tab is pressed
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.f, 8.f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(250.0f, 300.0f), ImVec2(-1.0f, 500.0f));
        addNodePopup(TextCursor);
        searchNodePopup(TextCursor);
        addPinPopup();
        readOnlyPopup();
        ImGui::PopStyleVar();

        ed::Resume();

        // Gather selected nodes / links - from ImGui Node Editor blueprints-example.cpp
        std::vector<ed::NodeId> selectedNodes;
        std::vector<ed::LinkId> selectedLinks;
        selectedNodes.resize(ed::GetSelectedObjectCount());
        selectedLinks.resize(ed::GetSelectedObjectCount());

        int nodeCount = ed::GetSelectedNodes(selectedNodes.data(), static_cast<int>(selectedNodes.size()));
        int linkCount = ed::GetSelectedLinks(selectedLinks.data(), static_cast<int>(selectedLinks.size()));

        selectedNodes.resize(nodeCount);
        selectedLinks.resize(linkCount);
        if (io2.KeyCtrl && io2.MouseDown[0])
        {
            _ctrlClick = true;
        }

        // Set current node based off of selected node
        if (selectedNodes.size() > 0)
        {
            int graphPos = findNode(int(selectedNodes[0].Get()));
            if (graphPos > -1)
            {
                // Only selected if its not the same as previously selected
                if (!_prevUiNode || (_prevUiNode->getName() != _graphNodes[graphPos]->getName()))
                {
                    _currUiNode = _graphNodes[graphPos];

                    // Update render material if needed
                    if (_currUiNode->getNode())
                    {
                        setRenderMaterial(_currUiNode);
                    }
                    else if (_currUiNode->getNodeGraph() || _currUiNode->getOutput())
                    {
                        setRenderMaterial(_currUiNode);
                    }
                    _prevUiNode = _currUiNode;
                }
            }
        }

        // Check if keyboard shortcuts for copy/cut/paste have been used
        if (ed::BeginShortcut())
        {
            if (ed::AcceptCopy())
            {
                _copiedNodes.clear();
                for (ed::NodeId selected : selectedNodes)
                {
                    int pos = findNode((int) selected.Get());
                    if (pos >= 0)
                    {
                        _copiedNodes.emplace(_graphNodes[pos], nullptr);
                    }
                }
            }
            else if (ed::AcceptCut())
            {
                if (!readOnly())
                {
                    _copiedNodes.clear();

                    // Same as copy but remove from graphNodes
                    for (ed::NodeId selected : selectedNodes)
                    {
                        int pos = findNode((int) selected.Get());
                        if (pos >= 0)
                        {
                            _copiedNodes.emplace(_graphNodes[pos], nullptr);
                        }
                    }
                    _isCut = true;
                }
                else
                {
                    _popup = true;
                }
            }
            else if (ed::AcceptPaste())
            {
                if (!readOnly())
                {
                    for (auto iter = _copiedNodes.begin(); iter != _copiedNodes.end(); ++iter)
                    {
                        copyUiNode(iter->first);
                        _addNewNode = true;
                    }
                }
                else
                {
                    _popup = true;
                }
            }
        }

        // Set y-position of first node
        std::vector<int> outputNum = createNodes(_isNodeGraph);

        // Address copy information if applicable and relink graph if a new node has been added
        if (_addNewNode)
        {
            copyInputs();
            linkGraph();
            ImVec2 canvasPos = ed::ScreenToCanvas(mousePos);

            // Place the copied nodes or the individual new nodes
            if (!_copiedNodes.empty())
            {
                positionPasteBin(canvasPos);
            }
            else if (!_graphNodes.empty())
            {
                ed::SetNodePosition(_graphNodes.back()->getId(), canvasPos);
            }
            _copiedNodes.clear();
            _addNewNode = false;

            if (_pinIdToLinkFrom != ed::PinId() && _pinIdToLinkTo != ed::PinId())
            {
                // we have a link to draw for the newly created node.
                if (checkCanAddLink(_pinIdToLinkFrom, _pinIdToLinkTo))
                {
                    addLink(_pinIdToLinkFrom, _pinIdToLinkTo);
                }

                _pinIdToLinkTo = ed::PinId();
                _pinIdToLinkFrom = ed::PinId();
            }
        }

        // Layout and link graph during the initial call of drawGraph
        if (_initial || _autoLayout)
        {
            _currLinks.clear();
            float y = 0.f;
            _levelMap = std::unordered_map<int, std::vector<UiNodePtr>>();

            // Start layout with output or material nodes since layout algorithm works right to left
            for (int outN : outputNum)
            {
                layoutPosition(_graphNodes[outN], ImVec2(1200.f, y), true, 0);
                y += 350;
            }

            // If there are no output or material nodes but the nodes have position layout each individual node
            if (_graphNodes.size() > 0)
            {

                if (outputNum.size() == 0 && _graphNodes[0]->getElement())
                {
                    for (UiNodePtr node : _graphNodes)
                    {
                        layoutPosition(node, ImVec2(0, 0), true, 0);
                    }
                }
            }
            linkGraph();
            findYSpacing(0.f);
            layoutInputs();

            // Automatically frame node graph upon loading
            ed::NavigateToContent();
        }
        if (_delete)
        {
            linkGraph();

            _delete = false;
        }
        connectLinks();

        // Set to false after initial layout so that nodes can be moved
        _initial = false;
        _autoLayout = false;

        // Start the session with content centered
        if (ImGui::GetFrameCount() == 2)
        {
            ed::NavigateToContent(0.0f);
        }

        // Delete selected nodes and their links if delete key is pressed
        // or if the shortcut for cut is used
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootWindow))
        {
            if (ImGui::IsKeyReleased(ImGuiKey_Delete) || ImGui::IsKeyReleased(ImGuiKey_Backspace) || _isCut)
            {
                if (selectedNodes.size() > 0)
                {
                    _frameCount = ImGui::GetFrameCount();
                    _renderer->setMaterialCompilation(true);
                    for (ed::NodeId id : selectedNodes)
                    {

                        if (int(id.Get()) > 0)
                        {
                            int pos = findNode(int(id.Get()));
                            if (pos >= 0 && !readOnly())
                            {
                                deleteNode(_graphNodes[pos]);
                                _delete = true;
                                ed::DeselectNode(id);
                                ed::DeleteNode(id);
                                _currUiNode = nullptr;
                            }
                            else if (readOnly())
                            {
                                _popup = true;
                            }
                        }
                    }
                    linkGraph();
                }
                else if (selectedLinks.size() > 0)
                {
                    _frameCount = ImGui::GetFrameCount();
                    _renderer->setMaterialCompilation(true);
                    for (ed::LinkId id : selectedLinks)
                    {
                        if (int(id.Get()) > 0 && !readOnly())
                        {
                            deleteLink(id);
                            _delete = true;
                            ed::DeselectLink(id);
                            ed::DeleteLink(id);
                            _currUiNode = nullptr;
                        }
                        else if (readOnly())
                        {
                            _popup = true;
                        }
                    }
                    linkGraph();
                }
                _isCut = false;
            }

            // Hotkey to frame selected node(s)
            if (ImGui::IsKeyReleased(ImGuiKey_F) && !_fileDialogSave.isOpened())
            {
                ed::NavigateToSelection();
            }

            // Go back up from inside a subgraph
            if (ImGui::IsKeyReleased(ImGuiKey_U) && (!ImGui::IsPopupOpen("add node")) && (!ImGui::IsPopupOpen("search")) && !_fileDialogSave.isOpened())
            {
                upNodeGraph();
            }
        }

        // Add new link
        if (ed::BeginCreate())
        {
            ed::PinId startPinId, endPinId, filterPinId;
            if (ed::QueryNewLink(&startPinId, &endPinId))
            {
                if (!readOnly())
                {
                    if (checkCanAddLink(startPinId, endPinId))
                    {
                        if (ed::AcceptNewItem())
                        {
                            addLink(startPinId, endPinId);
                        }
                    }
                    else
                    {
                        ed::RejectNewItem();
                    }
                }
                else
                {
                    _popup = true;
                }
            }
            if (ed::QueryNewNode(&filterPinId))
            {
                if (getPin(filterPinId)->_type != "null")
                {
                    _pinFilterType = getPin(filterPinId)->_type;

                    if (UiPinPtr filterPin = getPin(filterPinId); filterPin && filterPin->_kind == ed::PinKind::Input)
                    {
                        _pinIdToLinkTo = filterPinId;
                        _pinIdToLinkFrom = ed::PinId();
                    }
                    else
                    {
                        _pinIdToLinkTo = ed::PinId();
                        _pinIdToLinkFrom = filterPinId;
                    }
                }

                showLabel("Release Mouse to Add a New Node", ImColor(50, 50, 50, 255));
            }
        }
        else
        {
            _pinFilterType = mx::EMPTY_STRING;
        }
        ed::EndCreate();

        // Delete link
        if (ed::BeginDelete())
        {
            ed::LinkId deletedLinkId;
            while (ed::QueryDeletedLink(&deletedLinkId))
            {
                if (!readOnly())
                {
                    deleteLink(deletedLinkId);
                }
                else
                {
                    _popup = true;
                }
            }
        }
        ed::EndDelete();
    }

    // Dive into a node that has a subgraph
    ed::NodeId clickedNode = ed::GetDoubleClickedNode();
    if (clickedNode.Get() > 0)
    {
        if (_currUiNode != nullptr)
        {
            if (_currUiNode->getNode() != nullptr)
            {
                mx::InterfaceElementPtr impl = _currUiNode->getNode()->getImplementation();

                // Only dive if current node is a node graph
                if (impl && impl->isA<mx::NodeGraph>())
                {
                    savePosition();
                    _graphStack.push(_graphNodes);
                    _pinStack.push(_currPins);
                    _sizeStack.push(_graphTotalSize);
                    mx::NodeGraphPtr implGraph = impl->asA<mx::NodeGraph>();
                    _initial = true;
                    _graphNodes.clear();
                    ed::DeselectNode(_currUiNode->getId());
                    _currUiNode = nullptr;
                    _currGraphElem = implGraph;
                    if (readOnly())
                    {
                        std::string graphName = implGraph->getName() + " (Read Only)";
                        _currGraphName.push_back(graphName);
                        _popup = true;
                    }
                    else
                    {

                        _currGraphName.push_back(implGraph->getName());
                    }
                    buildUiNodeGraph(implGraph);
                    ed::NavigateToContent();
                }
            }
            else if (_currUiNode->getNodeGraph() != nullptr)
            {
                savePosition();
                _graphStack.push(_graphNodes);
                _pinStack.push(_currPins);
                _sizeStack.push(_graphTotalSize);
                mx::NodeGraphPtr implGraph = _currUiNode->getNodeGraph();
                _initial = true;
                _graphNodes.clear();
                _isNodeGraph = true;
                setRenderMaterial(_currUiNode);
                ed::DeselectNode(_currUiNode->getId());
                _currUiNode = nullptr;
                _currGraphElem = implGraph;
                if (readOnly())
                {

                    std::string graphName = implGraph->getName() + " (Read Only)";
                    _currGraphName.push_back(graphName);
                    _popup = true;
                }
                else
                {
                    _currGraphName.push_back(implGraph->getName());
                }
                buildUiNodeGraph(implGraph);
                ed::NavigateToContent();
            }
        }
    }

    shaderPopup();
    if (ImGui::GetFrameCount() == (_frameCount + 2))
    {
        updateMaterials();
        _renderer->setMaterialCompilation(false);
    }

    ed::Suspend();
    _fileDialogSave.display();

    // Save file
    if (_fileDialogSave.hasSelected())
    {
        std::string message;
        if (!_graphDoc->validate(&message))
        {
            std::cerr << "*** Validation warnings for " << _materialFilename.getBaseName() << " ***" << std::endl;
            std::cerr << message;
        }
        _materialFilename = _fileDialogSave.getSelected();
        ed::Resume();
        savePosition();

        saveDocument(_materialFilename);
        _fileDialogSave.clearSelected();
    }
    else
    {
        ed::Resume();
    }

    ed::End();
    ImGui::End();

    _fileDialog.display();

    // Create and load document from selected file
    if (_fileDialog.hasSelected())
    {
        mx::FilePath fileName = _fileDialog.getSelected();
        _currGraphName.clear();
        std::string graphName = fileName.getBaseName();
        _currGraphName.push_back(graphName.substr(0, graphName.length() - 5));
        _graphDoc = loadDocument(fileName);

        _initial = true;
        buildUiBaseGraph(_graphDoc);
        _currGraphElem = _graphDoc;
        _prevUiNode = nullptr;
        _fileDialog.clearSelected();

        _renderer->setDocument(_graphDoc);
        _renderer->updateMaterials(nullptr);
    }

    _fileDialogGeom.display();
    if (_fileDialogGeom.hasSelected())
    {
        mx::FilePath fileName = _fileDialogGeom.getSelected();
        _fileDialogGeom.clearSelected();
        _renderer->loadMesh(fileName);
        _renderer->updateMaterials(nullptr);
    }

    _fileDialogImage.display();
}

int Graph::findNode(int nodeId)
{
    int count = 0;
    for (size_t i = 0; i < _graphNodes.size(); i++)
    {
        if (_graphNodes[i]->getId() == nodeId)
        {
            return count;
        }
        count++;
    }
    return -1;
}

bool Graph::edgeExists(UiEdge newEdge)
{
    if (_currEdge.size() > 0)
    {
        for (UiEdge edge : _currEdge)
        {
            if (edge.getDown()->getId() == newEdge.getDown()->getId())
            {
                if (edge.getUp()->getId() == newEdge.getUp()->getId())
                {
                    if (edge.getInput() == newEdge.getInput())
                    {
                        return true;
                    }
                }
            }
            else if (edge.getUp()->getId() == newEdge.getDown()->getId())
            {
                if (edge.getDown()->getId() == newEdge.getUp()->getId())
                {
                    if (edge.getInput() == newEdge.getInput())
                    {
                        return true;
                    }
                }
            }
        }
    }
    else
    {
        return false;
    }
    return false;
}

bool Graph::linkExists(Link newLink)
{
    for (const auto& link : _currLinks)
    {
        if (link._startAttr == newLink._startAttr)
        {
            if (link._endAttr == newLink._endAttr)
            {
                return true;
            }
        }
        else if (link._startAttr == newLink._endAttr)
        {
            if (link._endAttr == newLink._startAttr)
            {
                return true;
            }
        }
    }
    return false;
}

void Graph::savePosition()
{
    for (UiNodePtr node : _graphNodes)
    {
        mx::ElementPtr elem = node->getElement();
        if (elem)
        {
            ImVec2 pos = ed::GetNodePosition(node->getId());
            pos.x /= DEFAULT_NODE_SIZE.x;
            pos.y /= DEFAULT_NODE_SIZE.y;
            elem->setAttribute(mx::Element::XPOS_ATTRIBUTE, std::to_string(pos.x));
            elem->setAttribute(mx::Element::YPOS_ATTRIBUTE, std::to_string(pos.y));
            if (elem->hasAttribute("nodedef"))
            {
                elem->removeAttribute("nodedef");
            }
        }
    }
}
void Graph::saveDocument(mx::FilePath filePath)
{
    if (filePath.getExtension() != mx::MTLX_EXTENSION)
    {
        filePath.addExtension(mx::MTLX_EXTENSION);
    }

    mx::DocumentPtr writeDoc = _graphDoc;

    // If requested, create a modified version of the document for saving.
    if (!_saveNodePositions)
    {
        writeDoc = _graphDoc->copy();
        for (mx::ElementPtr elem : writeDoc->traverseTree())
        {
            elem->removeAttribute(mx::Element::XPOS_ATTRIBUTE);
            elem->removeAttribute(mx::Element::YPOS_ATTRIBUTE);
        }
    }

    mx::XmlWriteOptions writeOptions;
    writeOptions.elementPredicate = getElementPredicate();
    mx::writeToXmlFile(writeDoc, filePath, &writeOptions);
}
