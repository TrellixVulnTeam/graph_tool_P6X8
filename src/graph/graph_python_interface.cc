// graph-tool -- a general graph modification and manipulation thingy
//
// Copyright (C) 2006-2015 Tiago de Paula Peixoto <tiago@skewed.de>
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 3
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <http://www.gnu.org/licenses/>.

#include "graph_filtering.hh"
#include "graph.hh"
#include "graph_python_interface.hh"

#include <boost/python.hpp>
#include <boost/python/stl_iterator.hpp>
#include <set>

using namespace std;
using namespace boost;
using namespace graph_tool;

namespace graph_tool
{

struct get_vertex_iterator
{
    template <class Graph>
    void operator()(Graph& g, python::object& pg,
                    python::object& iter) const
    {
        typedef typename graph_traits<Graph>::vertex_iterator vertex_iterator;
        iter = python::object(PythonIterator<PythonVertex,
                                             vertex_iterator>(pg, vertices(g)));
    }
};

python::object get_vertices(python::object g)
{
    GraphInterface& gi = python::extract<GraphInterface&>(g().attr("_Graph__graph"));
    python::object iter;
    run_action<>()(gi, std::bind(get_vertex_iterator(), placeholders::_1,
                                 std::ref(g),
                                 std::ref(iter)))();
    return iter;
}

struct get_vertex_soft
{
    template <class Graph>
    void operator()(Graph& g,
                    python::object& pg,
                    size_t i, python::object& v) const
    {
        v = python::object(PythonVertex(pg, vertex(i, g)));
    }
};

struct get_vertex_hard
{
    template <class Graph>
    void operator()(Graph& g, python::object& pg, size_t i,
                    python::object& v) const
    {
        size_t c = 0;
        typename graph_traits<Graph>::vertex_iterator vi, v_end;
        for (tie(vi, v_end) = vertices(g); vi != v_end; ++vi)
        {
            if (c == i)
            {
                v = python::object(PythonVertex(pg, *vi));
                return;
            }
            ++c;
        }
        v = python::object(PythonVertex(pg,
                                        graph_traits<Graph>::null_vertex()));
    }
};

python::object get_vertex(python::object g, size_t i)
{
    GraphInterface& gi = python::extract<GraphInterface&>(g().attr("_Graph__graph"));
    python::object v;
    if (gi.IsVertexFilterActive())
        run_action<>()(gi,
                       std::bind(get_vertex_hard(), placeholders::_1,
                                 std::ref(g), i, std::ref(v)))();
    else
        run_action<>()(gi,
                       std::bind(get_vertex_soft(), placeholders::_1,
                                 std::ref(g), i, std::ref(v)))();
    return v;
}

struct get_edge_iterator
{
    template <class Graph>
    void operator()(Graph& g, const python::object& pg, python::object& iter)
        const
    {
        typedef typename graph_traits<Graph>::edge_iterator edge_iterator;
        iter = python::object(PythonIterator<PythonEdge<Graph>,
                                             edge_iterator>(pg, edges(g)));
    }
};

python::object get_edges(python::object g)
{
    GraphInterface& gi = python::extract<GraphInterface&>(g().attr("_Graph__graph"));
    python::object iter;
    run_action<>()(gi,
                   std::bind(get_edge_iterator(), placeholders::_1,
                             std::ref(g), std::ref(iter)))();
    return iter;
}

python::object add_vertex(python::object g, size_t n)
{
    GraphInterface& gi = python::extract<GraphInterface&>(g().attr("_Graph__graph"));

    if (n > 1)
    {
        for (size_t i = 0; i < n; ++i)
            add_vertex(gi.GetGraph());
        return python::object();
    }
    return python::object(PythonVertex(g, add_vertex(gi.GetGraph())));
}


void remove_vertex(GraphInterface& gi, const python::object& oindex, bool fast)
{
    boost::multi_array_ref<int64_t,1> index = get_array<int64_t,1>(oindex);
    auto& g = gi.GetGraph();
    if (fast)
    {
        for (auto v : index)
            remove_vertex_fast(vertex(v, g), g);
    }
    else
    {
        for (auto v : index)
            remove_vertex(vertex(v, g), g);
    }
}

struct add_new_edge
{
    template <class Graph, class EdgeIndexMap>
    void operator()(Graph& g, python::object& pg, GraphInterface&,
                    const PythonVertex& s, const PythonVertex& t,
                    EdgeIndexMap, python::object& new_e) const
    {
        typename graph_traits<Graph>::edge_descriptor e =
            add_edge(s.GetDescriptor(), t.GetDescriptor(), g).first;
        new_e = python::object(PythonEdge<Graph>(pg, e));
    }
};

python::object add_edge(python::object g, const python::object& s,
                        const python::object& t)
{
    PythonVertex& src = python::extract<PythonVertex&>(s);
    PythonVertex& tgt = python::extract<PythonVertex&>(t);
    src.CheckValid();
    tgt.CheckValid();
    GraphInterface& gi = python::extract<GraphInterface&>(g().attr("_Graph__graph"));
    python::object new_e;
    run_action<>()(gi, std::bind(add_new_edge(), placeholders::_1, std::ref(g),
                                 std::ref(gi), src, tgt, gi.GetEdgeIndex(),
                                 std::ref(new_e)))();
    return new_e;
}

struct get_edge_descriptor
{
    template <class Graph>
    void operator()(const Graph&, const python::object& e,
                    typename GraphInterface::edge_t& edge,
                    bool& found)  const
    {
        PythonEdge<Graph>& pe = python::extract<PythonEdge<Graph>&>(e);
        pe.CheckValid();
        pe.SetValid(false);
        edge = pe.GetDescriptor();
        found = true;
    }
};

void remove_edge(GraphInterface& gi, const python::object& e)
{
    GraphInterface::edge_t de;
    bool found = false;
    run_action<>()(gi, std::bind(get_edge_descriptor(), placeholders::_1,
                                 std::ref(e), std::ref(de), std::ref(found)))();
    remove_edge(de, gi.GetGraph());
    if (!found)
        throw ValueException("invalid edge descriptor");
}

struct get_edge_dispatch
{
    template <class Graph>
    void operator()(Graph& g, const python::object& pg, size_t s, size_t t,
                    bool all_edges, boost::python::list& es) const
    {
        for (auto e : out_edges_range(vertex(s, g), g))
        {
            if (target(e, g) == vertex(t, g))
            {
                es.append(PythonEdge<Graph>(pg, e));
                if (!all_edges)
                    break;
            }
        }
    }
};

python::object get_edge(python::object g, size_t s, size_t t, bool all_edges)
{
    GraphInterface& gi = python::extract<GraphInterface&>(g().attr("_Graph__graph"));
    python::list es;
    run_action<>()(gi,
                   std::bind(get_edge_dispatch(), placeholders::_1,
                             std::ref(g), s, t, all_edges, std::ref(es)))();
    return es;
}


struct get_degree_map
{
    template <class Graph, class DegS, class Weight>
    void operator()(const Graph& g, python::object& odeg_map, DegS deg, Weight weight) const
    {
        typedef typename detail::get_weight_type<Weight>::type weight_t;
        typedef typename mpl::if_<std::is_same<weight_t, size_t>, int32_t, weight_t>::type deg_t;

        typedef typename property_map_type::apply<deg_t,
                                                  GraphInterface::vertex_index_map_t>::type
            map_t;

        map_t cdeg_map(get(vertex_index, g));
        typename map_t::unchecked_t deg_map = cdeg_map.get_unchecked(num_vertices(g));

        int i, N = num_vertices(g);
        #pragma omp parallel for default(shared) private(i) schedule(runtime) if (N > 100)
        for (i = 0; i < N; ++i)
        {
            typename graph_traits<Graph>::vertex_descriptor v = vertex(i, g);
            if (v == graph_traits<Graph>::null_vertex())
                continue;
            deg_map[v] = deg(v, g, weight);
        }

        odeg_map = python::object(PythonPropertyMap<map_t>(cdeg_map));
    }
};

python::object GraphInterface::DegreeMap(string deg, boost::any weight) const
{

    python::object deg_map;

    typedef mpl::push_back<edge_scalar_properties,
                           detail::no_weightS>::type weight_t;
    if (weight.empty())
        weight = detail::no_weightS();

    if (deg == "in")
        run_action<>()(const_cast<GraphInterface&>(*this),
                       std::bind(get_degree_map(), placeholders::_1,
                                 std::ref(deg_map), in_degreeS(), placeholders::_2), weight_t())
            (weight);
    else if (deg == "out")
        run_action<>()(const_cast<GraphInterface&>(*this),
                       std::bind(get_degree_map(), placeholders::_1,
                                 std::ref(deg_map), out_degreeS(), placeholders::_2), weight_t())
            (weight);
    else if (deg == "total")
        run_action<>()(const_cast<GraphInterface&>(*this),
                       std::bind(get_degree_map(), placeholders::_1,
                                 std::ref(deg_map), total_degreeS(), placeholders::_2), weight_t())
            (weight);
    return deg_map;
}

//
// Below are the functions with will properly register all the types to python,
// for every filter, type, etc.
//

// this will register all the Vertex/Edge classes to python
struct export_python_interface
{
    template <class Graph>
    void operator()(const Graph*, set<string>& v_iterators) const
    {
        using namespace boost::python;

        class_<PythonEdge<Graph>, bases<EdgeBase> >
            ("Edge", no_init)
            .def("source", &PythonEdge<Graph>::GetSource,
                 "Return the source vertex.")
            .def("target", &PythonEdge<Graph>::GetTarget,
                 "Return the target vertex.")
            .def("is_valid", &PythonEdge<Graph>::IsValid,
                 "Return whether the edge is valid.")
            .def("get_graph", &PythonEdge<Graph>::GetGraph,
                 "Return the graph to which the edge belongs.")
            .def("__str__", &PythonEdge<Graph>::GetString)
            .def("__hash__", &PythonEdge<Graph>::GetHash);

        typedef typename graph_traits<Graph>::vertex_iterator vertex_iterator;
        if (v_iterators.find(typeid(vertex_iterator).name()) ==
            v_iterators.end())
        {
            class_<PythonIterator<PythonVertex, vertex_iterator> >
                ("VertexIterator", no_init)
                .def("__iter__", objects::identity_function())
                .def("__next__", &PythonIterator<PythonVertex,
                                                 vertex_iterator>::Next)
                .def("next", &PythonIterator<PythonVertex,
                                             vertex_iterator>::Next);
            v_iterators.insert(typeid(vertex_iterator).name());
        }

        typedef typename graph_traits<Graph>::edge_iterator edge_iterator;
        class_<PythonIterator<PythonEdge<Graph>,
                              edge_iterator> >("EdgeIterator", no_init)
            .def("__iter__", objects::identity_function())
            .def("__next__", &PythonIterator<PythonEdge<Graph>,
                                         edge_iterator>::Next)
            .def("next", &PythonIterator<PythonEdge<Graph>,
                                         edge_iterator>::Next);

        typedef typename graph_traits<Graph>::out_edge_iterator
            out_edge_iterator;
        class_<PythonIterator<PythonEdge<Graph>,
                              out_edge_iterator> >("OutEdgeIterator", no_init)
            .def("__iter__", objects::identity_function())
            .def("__next__", &PythonIterator<PythonEdge<Graph>,
                                             out_edge_iterator>::Next)
            .def("next", &PythonIterator<PythonEdge<Graph>,
                                         out_edge_iterator>::Next);

        typedef typename graph_traits<Graph>::directed_category
            directed_category;
        typedef typename std::is_convertible<directed_category,
                                             boost::directed_tag>::type is_directed;
        if (is_directed::value)
        {
            typedef typename in_edge_iteratorS<Graph>::type in_edge_iterator;
            class_<PythonIterator<PythonEdge<Graph>,
                                  in_edge_iterator> >("InEdgeIterator", no_init)
                .def("__iter__", objects::identity_function())
                .def("__next__", &PythonIterator<PythonEdge<Graph>,
                                                 in_edge_iterator>::Next)
                .def("next", &PythonIterator<PythonEdge<Graph>,
                                             in_edge_iterator>::Next);
        }
    }
};

PythonPropertyMap<GraphInterface::vertex_index_map_t>
get_vertex_index(GraphInterface& g)
{
    return PythonPropertyMap<GraphInterface::vertex_index_map_t>
        (g.GetVertexIndex());
}

PythonPropertyMap<GraphInterface::edge_index_map_t>
do_get_edge_index(GraphInterface& g)
{
    return PythonPropertyMap<GraphInterface::edge_index_map_t>
        (g.GetEdgeIndex());
}

template <class ValueList>
struct add_edge_list
{
    template <class Graph>
    void operator()(Graph& g, python::object aedge_list,
                    python::object& eprops, bool& found) const
    {
        boost::mpl::for_each<ValueList>(std::bind(dispatch(), std::ref(g),
                                                  std::ref(aedge_list),
                                                  std::ref(eprops),
                                                  std::ref(found),
                                                  placeholders::_1));
    }

    struct dispatch
    {
        template <class Graph, class Value>
        void operator()(Graph& g, python::object& aedge_list,
                        python::object& oeprops, bool& found, Value) const
        {
            if (found)
                return;
            try
            {
                boost::multi_array_ref<Value, 2> edge_list = get_array<Value, 2>(aedge_list);

                if (edge_list.shape()[1] < 2)
                    throw GraphException("Second dimension in edge list must be of size (at least) two");

                typedef typename graph_traits<Graph>::edge_descriptor edge_t;
                vector<DynamicPropertyMapWrap<Value, edge_t>> eprops;
                python::stl_input_iterator<boost::any> iter(oeprops), end;
                for (; iter != end; ++iter)
                    eprops.emplace_back(*iter, writable_edge_properties());

                for (const auto& e : edge_list)
                {
                    size_t s = e[0];
                    size_t t = e[1];
                    while (s >= num_vertices(g) || t >= num_vertices(g))
                        add_vertex(g);
                    auto ne = add_edge(vertex(s, g), vertex(t, g), g).first;
                    for (size_t i = 0; i < e.size() - 2; ++i)
                    {
                        try
                        {
                            put(eprops[i], ne, e[i + 2]);
                        }
                        catch(bad_lexical_cast&)
                        {
                            throw ValueException("Invalid edge property value: " +
                                                 lexical_cast<string>(e[i + 2]));
                        }
                    }
                }
                found = true;
            }
            catch (invalid_numpy_conversion& e) {}
        }
    };
};

void do_add_edge_list(GraphInterface& gi, python::object aedge_list,
                      python::object eprops)
{
    typedef mpl::vector<bool, char, uint8_t, uint16_t, uint32_t, uint64_t,
                        int8_t, int16_t, int32_t, int64_t, uint64_t, double,
                        long double> vals_t;
    bool found = false;
    run_action<>()(gi, std::bind(add_edge_list<vals_t>(), placeholders::_1,
                                 aedge_list, std::ref(eprops),
                                 std::ref(found)))();
    if (!found)
        throw GraphException("Invalid type for edge list; must be two-dimensional with a scalar type");
}

template <class ValueList>
struct add_edge_list_hash
{
    template <class Graph, class VProp>
    void operator()(Graph& g, python::object aedge_list, VProp vmap,
                    bool& found, bool use_str, python::object& eprops) const
    {
        boost::mpl::for_each<ValueList>(std::bind(dispatch(), std::ref(g),
                                                  std::ref(aedge_list), std::ref(vmap),
                                                  std::ref(found), std::ref(eprops),
                                                  placeholders::_1));
        if (!found)
        {
            if (use_str)
                dispatch()(g, aedge_list, vmap, found, eprops, std::string());
            else
                dispatch()(g, aedge_list, vmap, found, eprops, python::object());
        }
    }

    struct dispatch
    {
        template <class Graph, class VProp, class Value>
        void operator()(Graph& g, python::object& aedge_list, VProp& vmap,
                        bool& found, python::object& oeprops, Value) const
        {
            if (found)
                return;
            try
            {
                boost::multi_array_ref<Value, 2> edge_list = get_array<Value, 2>(aedge_list);
                unordered_map<Value, size_t> vertices;

                if (edge_list.shape()[1] < 2)
                    throw GraphException("Second dimension in edge list must be of size (at least) two");

                typedef typename graph_traits<Graph>::edge_descriptor edge_t;
                vector<DynamicPropertyMapWrap<Value, edge_t>> eprops;
                python::stl_input_iterator<boost::any> iter(oeprops), end;
                for (; iter != end; ++iter)
                    eprops.emplace_back(*iter, writable_edge_properties());

                auto get_vertex = [&] (const Value& r) -> size_t
                    {
                        auto iter = vertices.find(r);
                        if (iter == vertices.end())
                        {
                            auto v = add_vertex(g);
                            vertices[r] = v;
                            vmap[v] = lexical_cast<typename property_traits<VProp>::value_type>(r);
                            return v;
                        }
                        return iter->second;
                    };

                for (const auto& e : edge_list)
                {
                    size_t s = get_vertex(e[0]);
                    size_t t = get_vertex(e[1]);
                    auto ne = add_edge(vertex(s, g), vertex(t, g), g).first;
                    for (size_t i = 0; i < e.size() - 2; ++i)
                    {
                        try
                        {
                            put(eprops[i], ne, e[i + 2]);
                        }
                        catch(bad_lexical_cast&)
                        {
                            throw ValueException("Invalid edge property value: " +
                                                 lexical_cast<string>(e[i + 2]));
                        }
                    }
                }
                found = true;
            }
            catch (invalid_numpy_conversion& e) {}
        }

        template <class Graph, class VProp>
        void operator()(Graph& g, python::object& edge_list, VProp& vmap,
                        bool& found, python::object& oeprops, std::string) const
        {
            if (found)
                return;
            try
            {
                unordered_map<std::string, size_t> vertices;

                typedef typename graph_traits<Graph>::edge_descriptor edge_t;
                vector<DynamicPropertyMapWrap<python::object, edge_t>> eprops;
                python::stl_input_iterator<boost::any> piter(oeprops), pend;
                for (; piter != pend; ++piter)
                    eprops.emplace_back(*piter, writable_edge_properties());

                auto get_vertex = [&] (const std::string& r) -> size_t
                    {
                        auto iter = vertices.find(r);
                        if (iter == vertices.end())
                        {
                            auto v = add_vertex(g);
                            vertices[r] = v;
                            vmap[v] = lexical_cast<typename property_traits<VProp>::value_type>(r);
                            return v;
                        }
                        return iter->second;
                    };

                python::stl_input_iterator<python::object> iter(edge_list), end;
                for (; iter != end; ++iter)
                {
                    const auto& row = *iter;

                    python::stl_input_iterator<python::object> eiter(row), eend;

                    size_t s = 0;
                    size_t t = 0;

                    typename graph_traits<Graph>::edge_descriptor e;
                    size_t i = 0;
                    for(; eiter != eend; ++eiter)
                    {
                        if (i >= eprops.size() + 2)
                            break;
                        const auto& val = *eiter;
                        switch (i)
                        {
                        case 0:
                            s = get_vertex(python::extract<std::string>(val));
                            while (s >= num_vertices(g))
                                add_vertex(g);
                            break;
                        case 1:
                            t = get_vertex(python::extract<std::string>(val));
                            while (t >= num_vertices(g))
                                add_vertex(g);
                            e = add_edge(vertex(s, g), vertex(t, g), g).first;
                            break;
                        default:
                            try
                            {
                                put(eprops[i - 2], e, val);
                            }
                            catch(bad_lexical_cast&)
                            {
                                throw ValueException("Invalid edge property value: " +
                                                     python::extract<string>(python::str(val))());
                            }
                        }
                        i++;
                    }
                }
                found = true;
            }
            catch (invalid_numpy_conversion& e) {}
        }

        template <class Graph, class VProp>
        void operator()(Graph& g, python::object& edge_list, VProp& vmap,
                        bool& found, python::object& oeprops, python::object) const
        {
            if (found)
                return;
            try
            {
                unordered_map<python::object, size_t> vertices;

                typedef typename graph_traits<Graph>::edge_descriptor edge_t;
                vector<DynamicPropertyMapWrap<python::object, edge_t>> eprops;
                python::stl_input_iterator<boost::any> piter(oeprops), pend;
                for (; piter != pend; ++piter)
                    eprops.emplace_back(*piter, writable_edge_properties());

                auto get_vertex = [&] (const python::object& r) -> size_t
                    {
                        auto iter = vertices.find(r);
                        if (iter == vertices.end())
                        {
                            auto v = add_vertex(g);
                            vertices[r] = v;
                            vmap[v] = python::extract<typename property_traits<VProp>::value_type>(r);
                            return v;
                        }
                        return iter->second;
                    };

                python::stl_input_iterator<python::object> iter(edge_list), end;
                for (; iter != end; ++iter)
                {
                    const auto& row = *iter;

                    python::stl_input_iterator<python::object> eiter(row), eend;

                    size_t s = 0;
                    size_t t = 0;

                    typename graph_traits<Graph>::edge_descriptor e;
                    size_t i = 0;
                    for(; eiter != eend; ++eiter)
                    {
                        if (i >= eprops.size() + 2)
                            break;
                        const auto& val = *eiter;
                        switch (i)
                        {
                        case 0:
                            s = get_vertex(val);
                            while (s >= num_vertices(g))
                                add_vertex(g);
                            break;
                        case 1:
                            t = get_vertex(val);
                            while (t >= num_vertices(g))
                                add_vertex(g);
                            e = add_edge(vertex(s, g), vertex(t, g), g).first;
                            break;
                        default:
                            try
                            {
                                put(eprops[i - 2], e, val);
                            }
                            catch(bad_lexical_cast&)
                            {
                                throw ValueException("Invalid edge property value: " +
                                                     python::extract<string>(python::str(val))());
                            }
                        }
                        i++;
                    }
                }
                found = true;
            }
            catch (invalid_numpy_conversion& e) {}
        }
    };
};

void do_add_edge_list_hashed(GraphInterface& gi, python::object aedge_list,
                             boost::any& vertex_map, bool is_str,
                             python::object eprops)
{
    typedef mpl::vector<bool, char, uint8_t, uint16_t, uint32_t, uint64_t,
                        int8_t, int16_t, int32_t, int64_t, uint64_t, double,
                        long double> vals_t;
    bool found = false;
    run_action<graph_tool::detail::all_graph_views, boost::mpl::true_>()
        (gi, std::bind(add_edge_list_hash<vals_t>(), placeholders::_1,
                       aedge_list, placeholders::_2, std::ref(found),
                       is_str, std::ref(eprops)),
         writable_vertex_properties())(vertex_map);
}


struct add_edge_list_iter
{
    template <class Graph>
    void operator()(Graph& g, python::object& edge_list,
                    python::object& oeprops) const
    {
        typedef typename graph_traits<Graph>::edge_descriptor edge_t;
        vector<DynamicPropertyMapWrap<python::object, edge_t>> eprops;
        python::stl_input_iterator<boost::any> piter(oeprops), pend;
        for (; piter != pend; ++piter)
            eprops.emplace_back(*piter, writable_edge_properties());

        python::stl_input_iterator<python::object> iter(edge_list), end;
        for (; iter != end; ++iter)
        {
            const auto& row = *iter;
            python::stl_input_iterator<python::object> eiter(row), eend;

            size_t s = 0;
            size_t t = 0;

            typename graph_traits<Graph>::edge_descriptor e;
            size_t i = 0;
            for(; eiter != eend; ++eiter)
            {
                if (i >= eprops.size() + 2)
                    break;
                const auto& val = *eiter;
                switch (i)
                {
                case 0:
                    s = python::extract<size_t>(val);
                    while (s >= num_vertices(g))
                        add_vertex(g);
                    break;
                case 1:
                    t = python::extract<size_t>(val);
                    while (t >= num_vertices(g))
                        add_vertex(g);
                    e = add_edge(vertex(s, g), vertex(t, g), g).first;
                    break;
                default:
                    try
                    {
                        put(eprops[i - 2], e, val);
                    }
                    catch(bad_lexical_cast&)
                    {
                        throw ValueException("Invalid edge property value: " +
                                             python::extract<string>(python::str(val))());
                    }
                }
                i++;
            }
        }
    }
};

void do_add_edge_list_iter(GraphInterface& gi, python::object edge_list,
                           python::object eprops)
{
    run_action<>()
        (gi, std::bind(add_edge_list_iter(), placeholders::_1,
                       std::ref(edge_list), std::ref(eprops)))();
}


} // namespace graph_tool

// register everything

void export_python_properties();

void export_python_interface()
{
    using namespace boost::python;

    class_<PythonVertex>
        ("Vertex", no_init)
        .def("__in_degree", &PythonVertex::GetInDegree,
             "Return the in-degree.")
        .def("__weighted_in_degree", &PythonVertex::GetWeightedInDegree,
             "Return the weighted in-degree.")
        .def("__out_degree", &PythonVertex::GetOutDegree,
             "Return the out-degree.")
        .def("__weighted_out_degree", &PythonVertex::GetWeightedOutDegree,
             "Return the weighted out-degree.")
        .def("in_edges", &PythonVertex::InEdges,
             "Return an iterator over the in-edges.")
        .def("out_edges", &PythonVertex::OutEdges,
             "Return an iterator over the out-edges.")
        .def("is_valid", &PythonVertex::IsValid,
             "Return whether the vertex is valid.")
        .def("get_graph", &PythonVertex::GetGraph,
             "Return the graph to which the vertex belongs.")
        .def("__str__", &PythonVertex::GetString)
        .def("__int__", &PythonVertex::GetIndex)
        .def("__hash__", &PythonVertex::GetHash);
    class_<EdgeBase>("EdgeBase", no_init);

    set<string> v_iterators;
    typedef boost::mpl::transform<graph_tool::detail::all_graph_views,
                                  boost::mpl::quote1<std::add_pointer> >::type graph_views;
    boost::mpl::for_each<graph_views>(std::bind(graph_tool::export_python_interface(),
                                      placeholders::_1, std::ref(v_iterators)));
    export_python_properties();
    def("new_vertex_property",
        &new_property<GraphInterface::vertex_index_map_t>);
    def("new_edge_property", &new_property<GraphInterface::edge_index_map_t>);
    def("new_graph_property",
        &new_property<ConstantPropertyMap<size_t,graph_property_tag> >);

    def("get_vertex", get_vertex);
    def("get_vertices", get_vertices);
    def("get_edges", get_edges);
    def("add_vertex", graph_tool::add_vertex);
    def("add_edge", graph_tool::add_edge);
    def("remove_vertex", graph_tool::remove_vertex);
    def("remove_edge", graph_tool::remove_edge);
    def("add_edge_list", graph_tool::do_add_edge_list);
    def("add_edge_list_hashed", graph_tool::do_add_edge_list_hashed);
    def("add_edge_list_iter", graph_tool::do_add_edge_list_iter);
    def("get_edge", get_edge);

    def("get_vertex_index", get_vertex_index);
    def("get_edge_index", do_get_edge_index);
}
