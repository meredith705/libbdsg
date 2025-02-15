//
//  test_libbdsg.cpp
//  
// Contains tests for the data structures in libbdsg
//

#include <stdio.h>
#include <iostream>
#include <vector>
#include <cassert>
#include <unordered_set>
#include <random>
#include <sstream>
#include <thread>
#include <deque>
#include <functional>
#include <stdexcept>

#include <sys/stat.h>

#include "bdsg/odgi.hpp"
#include "bdsg/packed_graph.hpp"
#include "bdsg/hash_graph.hpp"
#include "bdsg/internal/packed_structs.hpp"
#include "bdsg/internal/mapped_structs.hpp"
#include "bdsg/internal/mmap_backend.hpp"
#include "bdsg/overlays/path_position_overlays.hpp"
#include "bdsg/overlays/packed_path_position_overlay.hpp"
#include "bdsg/overlays/vectorizable_overlays.hpp"
#include "bdsg/overlays/packed_subgraph_overlay.hpp"


using namespace bdsg;
using namespace handlegraph;
using namespace std;

//#define debug_at


/**
 * We define this template to test the mmap-managing base
 * class independently.
 */
template<typename Item>
class MmapArray : public MmapBackend {
public:

    MmapArray() = default;
    
    uint32_t get_magic_number() const {
        // We must define a magic number for this type.
        return 0x12345678;
    }
    
    size_t size() const {
        return serialized_data_size() / sizeof(Item); 
    }
    
    void resize(size_t count) {
        serialized_data_resize(count * sizeof(Item));
    }
    
    Item& at(size_t index) {
        static_assert(sizeof(Item) % sizeof(char) == 0, "Items must be char-aligned");
        if (index >= size()) {
            throw std::out_of_range("Tried to access index " + std::to_string(index) +
                " in array of size " + std::to_string(size()));
        }
        auto base = serialized_data();
        auto found = (Item*)(base + (index * sizeof(Item)));
#ifdef debug_at
        std::cerr << "Got writable reference to item index " << index << " at " << (void*)found << std::endl;
#endif
        return *found;
    }
    
    Item& at(size_t index) const {
        static_assert(sizeof(Item) % sizeof(char) == 0, "Items must be char-aligned");
        if (index >= size()) {
            throw std::out_of_range("Tried to access index " + std::to_string(index) +
                " in array of size " + std::to_string(size()));
        }
        auto base = serialized_data();
        auto found = (Item*)(base + (index * sizeof(Item)));
#ifdef debug_at
        std::cerr << "Got read-only reference to item index " << index << " at " << (void*)found << std::endl;
#endif
        return *found;
    }
};

// Have helpers to store and check some test data

size_t mix(size_t in, size_t salt = 0) {
    return ((in * in + (in << 2)) ^ salt) + 1;
}

template<typename Vectorish>
void fill_to(Vectorish& data, size_t count, int64_t nonce) {
    for (size_t i = 0; i < count; i++) {
        data.at(i) = mix(i, nonce);
    }
}

template<typename Vectorish>
void verify_to(const Vectorish& data, size_t count, int64_t nonce) {
    if (count > data.size()) {
        throw std::runtime_error("Trying to check " + std::to_string(count) + " items but only " + std::to_string(data.size()) + " are available");
    }
    for (size_t i = 0; i < count; i++) {
        auto correct_value = mix(i, nonce);
        auto observed_value = data.at(i);
        if (observed_value != correct_value) {
            cerr << "At index " << i << " observed " << observed_value << " at " << &data.at(i) << " but expected " << correct_value << endl;
        }
        assert(observed_value == correct_value);
    }
}

/**
 * Given a resizeable two-level container of numbers, vigorously resize it and
 * its members and make sure they have the right values.
 */
template<typename TwoLevel>
void bother_vector(TwoLevel& storage) {

    vector<vector<int>> truth;
    
    auto check = [&]() {
        // Make sure the structure under test is holding the correct data.
        if (storage.size() != truth.size()) {
            std::cerr << "Structure under test has " << storage.size() << " items but should have " << truth.size() << std::endl;
            assert(storage.size() == truth.size());
        }
        for (size_t i = 0; i < truth.size(); i++) {
            if (storage.at(i).size() != truth.at(i).size()) {
                std::cerr << "Structure under test has " << storage.at(i).size()
                    << " items in item " << i << " but should have " << truth.size() << std::endl;
                assert(storage.at(i).size() == truth.at(i).size());
            }
            for (size_t j = 0; j < truth.at(i).size(); j++) {
                if (storage.at(i).at(j) != truth.at(i).at(j)) {
                    std::cerr << "Structure under test has " << storage.at(i).at(j)
                        << " at " << j << " address " << ((intptr_t) &storage.at(i).at(j))
                        << " in item " << i << " address " << ((intptr_t) &storage.at(i))
                        << " but should have " << truth.at(i).at(j) << std::endl;
                    assert(storage.at(i).at(j) == truth.at(i).at(j));
                }
            }
        }
    };

    size_t seed = 0;

    for (size_t iteration = 0; iteration < 2; iteration++) {
        truth.resize(0);
        storage.resize(0);
        check();
        
        for (size_t parent_size = 0; parent_size < 100; parent_size++) {
#ifdef debug_bother
            std::cerr << "Resize parent to " << parent_size << endl;
#endif
            truth.resize(parent_size);
            storage.resize(parent_size);
            check();
            
            for (size_t child = 0; child < parent_size; child++) {
                auto& truth_child = truth.at(child);
                auto& storage_child = storage.at(child);
                
                size_t child_size = seed % 100;
                seed = mix(seed);
                
                for (size_t i = 0; i <= std::min(child_size, (size_t)5); i++) {
                    // Resize 1 bigger a bunch
#ifdef debug_bother
                    std::cerr << "Resize child " << child << " of " << parent_size << " to " << i << endl;
#endif
                    truth_child.resize(i);
                    storage_child.resize(i);
#ifdef debug_bother
                    std::cerr << "Check after resize to " << i << endl;
#endif
                    check();
#ifdef debug_bother
                    std::cerr << "Completed check after resize to " << i << endl;
#endif
                }
                
                truth_child.resize(child_size);
                storage_child.resize(child_size);
                check();
                
#ifdef debug_bother
                std::cerr << "Fill in " << child_size << " items in child " << child << endl;
#endif
                
                for (size_t i = 0; i < child_size; i++) {
                    // Fill in with data
                    truth_child.at(i) = seed % 10000;
                    storage_child.at(i) = seed % 10000;
                    seed = mix(seed);
                }
                
                // Cut in half
#ifdef debug_bother
                std::cerr << "Resize child " << child << " of " << parent_size << " to " << child_size/2 << endl;
#endif
                truth_child.resize(child_size/2);
                storage_child.resize(child_size/2);
                check();
                
                // And increase by 10 with empty slots
#ifdef debug_bother
                std::cerr << "Resize child " << child << " of " << parent_size << " to " << (truth_child.size() + 10) << endl;
#endif
                truth_child.resize(truth_child.size() + 10);
                storage_child.resize(storage_child.size() + 10);
                check();
            }
            
            // Now make sure that after all that the structures are equal.
            check();
        }
    }
}

void test_mmap_backend() {
    
    
    // We're going to need a temporary file
    // This filename fill be filled in with the actual filename.
    char filename[] = "tmpXXXXXX";
    int tmpfd = mkstemp(filename);
    assert(tmpfd != -1);
    
    {
        // Make a test array
        MmapArray<int64_t> numbers;
        
        // We should start empty
        assert(numbers.size() == 0);
        
        // We should be able to expand.
        numbers.resize(100);
        assert(numbers.size() == 100);
        
        // We should be able to contract again.
        numbers.resize(0);
        assert(numbers.size() == 0);
        
        // TODO: If /proc/sys/vm/overcommit_memory is 1 and we use MAP_NORESERVE
        // internally, we could get arbitrarily big without really needing the RAM or swap.
        // This should be 1 PB
        //                     k       m       g       t       p
        size_t VERY_BIG = 1L * 1000L * 1000L * 1000L * 1000L * 1000L;
        //numbers.resize(VERY_BIG);
        //assert(numbers.size() == VERY_BIG);
        
        // We should be able to contract again.
        numbers.resize(0);
        assert(numbers.size() == 0);
        
        // We can actually hold data
        numbers.resize(10);
        fill_to(numbers, 10, 0);
        verify_to(numbers, 10, 0);
        
        // Data is preserved when we resize down
        numbers.resize(5);
        assert(numbers.size() == 5);
        verify_to(numbers, 5, 0);
        
        // Data is preserved when we resize up
        numbers.resize(20480);
        verify_to(numbers, 5, 0);
        fill_to(numbers, 20480, 0);
        verify_to(numbers, 20480, 0);
        
        {
            // Save it to a C++ stream
            stringstream strm;
            numbers.serialize(strm);
            
            // Load it back from the C++ stream
            strm.seekg(0);
            MmapArray<int64_t> reloaded;
            reloaded.deserialize(strm);
            
            // Make sure that's right
            verify_to(reloaded, 20480, 0);
            
            // Destroy copy
        }
        
        {
            // Open a pipe and get read and write ends.
            int pipefds[2];
            assert(pipe(pipefds) == 0);
            
            // Start a thread to read the pipe
            std::thread read_thread([](int fd) {
                // Read a copy from the pipe
                MmapArray<int64_t> reloaded;
                reloaded.deserialize(fd);
                
                // Make sure that's right
                verify_to(reloaded, 20480, 0);
                
                // CLose our end of the pipe.
                assert(close(fd) == 0);
                
                // Copy goes out of scope
            }, pipefds[0]);
            
            // Save to the write end of the pipe
            numbers.serialize(pipefds[1]);
            // Close our end of the pipe so the other end stops streaming.
            assert(close(pipefds[1]) == 0);
            
            // Join the thread
            read_thread.join();
        }
        
        // Data is preserved after pipe write
        verify_to(numbers, 20480, 0);
        
        // Data is preserved when we resize down after pipe write
        numbers.resize(120);
        verify_to(numbers, 120, 0);
        // And back up again
        numbers.resize(20480);
        verify_to(numbers, 120, 0);
        fill_to(numbers, 20480, 0);
        verify_to(numbers, 20480, 0);
        
        // Write out to the temp file FD
        numbers.serialize(tmpfd);
        
        // Make sure it's still right
        verify_to(numbers, 20480, 0);
        
        {
            // Load it in another copy, from FD
            MmapArray<int64_t> reloaded;
            reloaded.deserialize(tmpfd);
            
            // Make sure that's right
            verify_to(reloaded, 20480, 0);
            
            // Destroy copy
        }
        
        {
            // Load it in another copy, from filename
            MmapArray<int64_t> reloaded;
            reloaded.deserialize(std::string(filename));
            
            // Make sure that's right
            verify_to(reloaded, 20480, 0);
            
            // Destroy copy
        }
        
        // Modify the original in place
        numbers.resize(40960);
        fill_to(numbers, 40960, 1);
        
        {
            // Load it in a new copy
            MmapArray<int64_t> reloaded;
            reloaded.deserialize(std::string(filename));
            
            // Make sure the modification took
            verify_to(reloaded, 40960, 1);
            
            // Destroy copy
        }
        
        {
            // Load it in a copy
            MmapArray<int64_t> reloaded;
            reloaded.deserialize(std::string(filename));
            
            // Sever the copy connection
            reloaded.dissociate();
            
            // Modify the copy
            reloaded.resize(4096);
            fill_to(reloaded, 4096, 3);
            
            // Make sure original is the same.
            verify_to(numbers, 40960, 1);
            
            // Destroy copy
        }
        
        {
            // Load it in a copy
            MmapArray<int64_t> reloaded;
            reloaded.deserialize(std::string(filename));
            
            // Sever the copy connection
            reloaded.dissociate();
            
            // Modify the copy
            reloaded.resize(4096);
            fill_to(reloaded, 4096, 3);
            
            // Make sure original is the same.
            verify_to(numbers, 40960, 1);
            
            // Destroy copy
        }
        
        // Make sure original is the same.
        verify_to(numbers, 40960, 1);
        
        // Sever connection on original
        numbers.dissociate();
        
        // Modify it again
        numbers.resize(12);
        fill_to(numbers, 12, 2);
        
        // Destroy it, because modifications to the file by other
        // non-dissociated loaders may put it into an undefined state.
    }
    
    {
        // Load it in a copy
        MmapArray<int64_t> reloaded;
        reloaded.deserialize(std::string(filename));
        
        // Make sure the last modification is not visible
        verify_to(reloaded, 40960, 1);
        
        // Modify the copy
        reloaded.resize(4096);
        fill_to(reloaded, 4096, 3);
    
        {
            // Load another copy
            MmapArray<int64_t> rereloaded;
            rereloaded.deserialize(std::string(filename));
            
            // Make sure the modification is visible
            verify_to(rereloaded, 4096, 3);
            
            // Discard it
        }
        
        // Sever the connection on the copy
        reloaded.dissociate();
       
        // Modify the copy
        reloaded.resize(4096);
        fill_to(reloaded, 4096, 4);
        
        {
            // Load another copy
            MmapArray<int64_t> rereloaded;
            rereloaded.deserialize(std::string(filename));
            
            // Make sure the modification is not visible
            verify_to(rereloaded, 4096, 3);
            
            // Discard it
        }
        
        // Discard loaded copy
    }
    
    
    // Make the backing file read-only, so reads won't have a write-back link.
    assert(fchmod(tmpfd, 0400) == 0);
    
    {
        // Load a copy
        MmapArray<int64_t> reloaded;
        reloaded.deserialize(std::string(filename));
        
        // Modify the copy without dissociating
        reloaded.resize(512);
        fill_to(reloaded, 512, 5);
        
        // Discard it
    }
    
    {
        // Load another copy
        MmapArray<int64_t> reloaded;
        reloaded.deserialize(std::string(filename));
        
        // Make sure the modification to the read-only file is not visible
        verify_to(reloaded, 4096, 3);
        
        // Discard it
    }
    
    // Close our temp file, which should not affect the link.
    // It also should work; the object isn't allowed to tamper with our
    // file descriptor.
    assert(close(tmpfd) == 0);
    
    // Try and delete the file out of the directory.
    unlink(filename);
    
    cerr << "MmapBackend tests successful!" << endl;
}

void test_mapped_structs() {
    {
    
        using T = big_endian<int64_t>;
        using A = bdsg::yomo::Allocator<T>;
        using V = CompatVector<T, A>;
        // Make a thing to hold onto a test array.
        bdsg::yomo::UniqueMappedPointer<V> numbers_holder;
        
        // Construct it
        numbers_holder.construct("GATTACA");
        
        { 
        
            // Get a reference to it, which will be valid unless we save() or something
            auto& vec1 = *numbers_holder;
            
            // We should start empty
            assert(vec1.size() == 0);
            
            // We should be able to expand.
            vec1.resize(100);
            assert(vec1.size() == 100);
            
            // And contract
            vec1.resize(10);
            assert(vec1.size() == 10);
            
            // And hold data
            fill_to(vec1, 10, 0);
            verify_to(vec1, 10, 0);
            
            // And expand again
            vec1.resize(100);
            assert(vec1.size() == 100);
            
            // And see the data
            verify_to(vec1, 10, 0);
            
            // And expand more
            vec1.resize(1000);
            assert(vec1.size() == 1000);
            
            // And see the data
            verify_to(vec1, 10, 0);
            
            // And hold more data
            fill_to(vec1, 1000, 1);
            verify_to(vec1, 1000, 1);
            
        }
            
        // We're going to need a temporary file
        // This filename fill be filled in with the actual filename.
        char filename[] = "tmpXXXXXX";
        int tmpfd = mkstemp(filename);
        assert(tmpfd != -1);
        
        numbers_holder.save(tmpfd);
        
        { 
            auto& vec2 = *numbers_holder;
            
            // We should have the same data
            assert(vec2.size() == 1000);
            verify_to(vec2, 1000, 1);
            
            // We should still be able to modify it.
            vec2.resize(4000);
            fill_to(vec2, 4000, 2);
            verify_to(vec2, 4000, 2);
        }
        
        numbers_holder.dissociate();
        
        {
            auto& vec3 = *numbers_holder;
            
            // After dissociating, we should be able to modify the vector
            vec3.resize(5);
            fill_to(vec3, 5, 3);
            verify_to(vec3, 5, 3);
        }
        
        numbers_holder.reset();
        numbers_holder.load(tmpfd, "GATTACA");
        
        {
            auto& vec4 = *numbers_holder;
            
            // When we reload we should see the last thing we wrote before dissociating.
            assert(vec4.size() == 4000);
            verify_to(vec4, 4000, 2);
        }
        
        close(tmpfd);
        unlink(filename);
        
    }
    
    {
    
        using T = big_endian<int64_t>;
        using A = bdsg::yomo::Allocator<T>;
        using V1 = CompatVector<T, A>;
        using A2 = bdsg::yomo::Allocator<V1>;
        using V2 = CompatVector<V1, A2>;
        // Make a thing to hold onto a test array of arrays.
        bdsg::yomo::UniqueMappedPointer<V2> numbers_holder_holder;
        
        numbers_holder_holder.construct();
    
        // Now do a vigorous test comparing to a normal vector
        bother_vector(*numbers_holder_holder);
    }
    
    cerr << "Mapped Structs tests successful!" << endl;
}
        


void test_serializable_handle_graphs() {
    
    vector<pair<SerializableHandleGraph*, SerializableHandleGraph*>> implementations;
    
    PackedGraph pg_out, pg_in;
    implementations.emplace_back(&pg_out, &pg_in);
    
    HashGraph hg_out, hg_in;
    implementations.emplace_back(&hg_out, &hg_in);
    
    ODGI og_out, og_in;
    implementations.emplace_back(&og_out, &og_in);
        
    for (pair<SerializableHandleGraph*, SerializableHandleGraph*> implementation : implementations) {
        
        MutablePathMutableHandleGraph* build_graph = dynamic_cast<MutablePathMutableHandleGraph*>(implementation.first);
        PathHandleGraph* check_graph = dynamic_cast<PathHandleGraph*>(implementation.second);
        SerializableHandleGraph* serialize_graph = implementation.first;
        SerializableHandleGraph* deserialize_graph = implementation.second;
        
        handle_t h1 = build_graph->create_handle("GATT");
        handle_t h2 = build_graph->create_handle("TTGA");
        handle_t h3 = build_graph->create_handle("T");
        handle_t h4 = build_graph->create_handle("CA");
        
        build_graph->create_edge(h1, h2);
        build_graph->create_edge(h1, build_graph->flip(h3));
        build_graph->create_edge(h2, h3);
        build_graph->create_edge(build_graph->flip(h3), h4);
        
        path_handle_t p = build_graph->create_path_handle("path");
        build_graph->append_step(p, h1);
        build_graph->append_step(p, h2);
        build_graph->append_step(p, h4);
        
        stringstream strm;
        
        serialize_graph->serialize(strm);
        strm.seekg(0);
        deserialize_graph->deserialize(strm);
        
        assert(build_graph->get_node_count() == check_graph->get_node_count());
        assert(build_graph->get_edge_count() == check_graph->get_edge_count());
        assert(build_graph->get_path_count() == check_graph->get_path_count());
        
        for (handle_t h : {h1, h2, h3, h4}) {
            assert(check_graph->has_node(build_graph->get_id(h)));
            assert(check_graph->get_sequence(check_graph->get_handle(build_graph->get_id(h))) == build_graph->get_sequence(h));
        }
        
        assert(check_graph->get_step_count(check_graph->get_path_handle(build_graph->get_path_name(p))) == build_graph->get_step_count(p));
    }
    
    cerr << "SerializableHandleGraph tests successful!" << endl;
}

void test_deletable_handle_graphs() {
    
    // first batch of tests
    {
        vector<DeletableHandleGraph*> implementations;

        // Add implementations

        PackedGraph pg;
        implementations.push_back(&pg);

        HashGraph hg;
        implementations.push_back(&hg);

        ODGI og;
        implementations.push_back(&og);

        // And test them

        for (DeletableHandleGraph* implementation : implementations) {

            DeletableHandleGraph& graph = *implementation;

            assert(graph.get_node_count() == 0);

            handle_t h = graph.create_handle("ATG", 2);

            // DeletableHandleGraph has correct structure after creating a node
            {
                assert(graph.get_sequence(h) == "ATG");
                assert(graph.get_sequence(graph.flip(h)) == "CAT");
                assert(graph.get_base(h, 1) == 'T');
                assert(graph.get_base(graph.flip(h), 2) == 'T');
                assert(graph.get_subsequence(h, 1, 3) == "TG");
                assert(graph.get_subsequence(graph.flip(h), 0, 2) == "CA");
                assert(graph.get_length(h) == 3);
                assert(graph.has_node(graph.get_id(h)));
                assert(!graph.has_node(graph.get_id(h) + 1));

                assert(graph.get_handle(graph.get_id(h)) == h);
                assert(!graph.get_is_reverse(h));
                assert(graph.get_is_reverse(graph.flip(h)));

                assert(graph.get_node_count() == 1);
                assert(graph.min_node_id() == graph.get_id(h));
                assert(graph.max_node_id() == graph.get_id(h));
                assert(graph.get_total_length() == 3);
                assert(graph.get_edge_count() == 0);

                graph.follow_edges(h, true, [](const handle_t& prev) {
                    assert(false);
                    return true;
                });
                graph.follow_edges(h, false, [](const handle_t& next) {
                    assert(false);
                    return true;
                });
            }

            handle_t h2 = graph.create_handle("CT", 1);

            // DeletableHandleGraph has correct structure after creating a node at the beginning of ID space
            {

                assert(graph.get_sequence(h2) == "CT");
                assert(graph.get_sequence(graph.flip(h2)) == "AG");
                assert(graph.get_base(h2, 1) == 'T');
                assert(graph.get_base(graph.flip(h2), 0) == 'A');
                assert(graph.get_subsequence(h2, 1, 10) == "T");
                assert(graph.get_subsequence(graph.flip(h2), 0, 2) == "AG");
                assert(graph.get_length(h2) == 2);
                assert(graph.has_node(graph.get_id(h2)));
                assert(!graph.has_node(max(graph.get_id(h), graph.get_id(h2)) + 1));

                assert(graph.get_handle(graph.get_id(h2)) == h2);

                assert(graph.get_node_count() == 2);
                assert(graph.min_node_id() == graph.get_id(h2));
                assert(graph.max_node_id() == graph.get_id(h));
                assert(graph.get_total_length() == 5);
                assert(graph.get_edge_count() == 0);

                graph.follow_edges(h2, true, [](const handle_t& prev) {
                    assert(false);
                    return true;
                });
                graph.follow_edges(h2, false, [](const handle_t& next) {
                    assert(false);
                    return true;
                });
            }

            // creating and accessing a node at the end of ID space

            handle_t h3 = graph.create_handle("GAC", 4);

            // DeletableHandleGraph has correct structure after creating a node at the end of ID space
            {
                assert(graph.get_sequence(h3) == "GAC");
                assert(graph.get_sequence(graph.flip(h3)) == "GTC");
                assert(graph.get_base(h3, 1) == 'A');
                assert(graph.get_base(graph.flip(h3), 0) == 'G');
                assert(graph.get_subsequence(h3, 1, 1) == "A");
                assert(graph.get_subsequence(graph.flip(h3), 0, 5) == "GTC");
                assert(graph.get_length(h3) == 3);

                assert(graph.get_handle(graph.get_id(h3)) == h3);

                assert(graph.get_node_count() == 3);
                assert(graph.min_node_id() == graph.get_id(h2));
                assert(graph.max_node_id() == graph.get_id(h3));
                assert(graph.get_total_length() == 8);
                assert(graph.get_edge_count() == 0);

                graph.follow_edges(h3, true, [](const handle_t& prev) {
                    assert(false);
                    return true;
                });
                graph.follow_edges(h3, false, [](const handle_t& next) {
                    assert(false);
                    return true;
                });
            }


            // creating and accessing in the middle of ID space

            handle_t h4 = graph.create_handle("T", 3);

            // DeletableHandleGraph has correct structure after creating a node in the middle of ID space
            {
                assert(graph.get_sequence(h4) == "T");
                assert(graph.get_sequence(graph.flip(h4)) == "A");
                assert(graph.get_length(h4) == 1);

                assert(graph.get_handle(graph.get_id(h4)) == h4);

                assert(graph.get_node_count() == 4);
                assert(graph.min_node_id() == graph.get_id(h2));
                assert(graph.max_node_id() == graph.get_id(h3));
                assert(graph.get_total_length() == 9);
                assert(graph.get_edge_count() == 0);

                graph.follow_edges(h4, true, [](const handle_t& prev) {
                    assert(false);
                    return true;
                });
                graph.follow_edges(h4, false, [](const handle_t& next) {
                    assert(false);
                    return true;
                });
            }

            graph.create_edge(h, h2);

            bool found1 = false, found2 = false, found3 = false, found4 = false;
            int count1 = 0, count2 = 0, count3 = 0, count4 = 0;

            // DeletableHandleGraph has correct structure after creating an edge
            {
                assert(graph.get_edge_count() == 1);

                graph.follow_edges(h, false, [&](const handle_t& next) {
                    if (next == h2) {
                        found1 = true;
                    }
                    count1++;
                    return true;
                });
                graph.follow_edges(h2, true, [&](const handle_t& prev) {
                    if (prev == h) {
                        found2 = true;
                    }
                    count2++;
                    return true;
                });
                graph.follow_edges(graph.flip(h), true, [&](const handle_t& prev) {
                    if (prev == graph.flip(h2)) {
                        found3 = true;
                    }
                    count3++;
                    return true;
                });
                graph.follow_edges(graph.flip(h2), false, [&](const handle_t& next) {
                    if (next == graph.flip(h)) {
                        found4 = true;
                    }
                    count4++;
                    return true;
                });
                assert(count1 == 1);
                assert(count2 == 1);
                assert(count3 == 1);
                assert(count4 == 1);
                assert(found1);
                assert(found2);
                assert(found3);
                assert(found4);

                count1 = count2 = count3 = count4 = 0;
                found1 = found2 = found3 = found4 = false;
            }

            graph.create_edge(h, graph.flip(h3));

            bool found5 = false, found6 = false, found7 = false, found8 = false;
            int count5 = 0, count6 = 0;

            // DeletableHandleGraph has correct structure after creating an edge with a traversal
            {
                assert(graph.get_edge_count() == 2);

                graph.follow_edges(h, false, [&](const handle_t& next) {
                    if (next == h2) {
                        found1 = true;
                    }
                    else if (next == graph.flip(h3)) {
                        found2 = true;
                    }
                    count1++;
                    return true;
                });
                graph.follow_edges(graph.flip(h), true, [&](const handle_t& prev) {
                    if (prev == graph.flip(h2)) {
                        found3 = true;
                    }
                    else if (prev == h3) {
                        found4 = true;
                    }
                    count2++;
                    return true;
                });
                graph.follow_edges(h2, true, [&](const handle_t& prev) {
                    if (prev == h) {
                        found5 = true;
                    }
                    count3++;
                    return true;
                });
                graph.follow_edges(graph.flip(h2), false, [&](const handle_t& next) {
                    if (next == graph.flip(h)) {
                        found6 = true;
                    }
                    count4++;
                    return true;
                });
                graph.follow_edges(graph.flip(h3), true, [&](const handle_t& prev) {
                    if (prev == h) {
                        found7 = true;
                    }
                    count5++;
                    return true;
                });
                graph.follow_edges(h3, false, [&](const handle_t& next) {
                    if (next == graph.flip(h)) {
                        found8 = true;
                    }
                    count6++;
                    return true;
                });
                assert(count1 == 2);
                assert(count2 == 2);
                assert(count3 == 1);
                assert(count4 == 1);
                assert(count5 == 1);
                assert(count6 == 1);
                assert(found1);
                assert(found2);
                assert(found3);
                assert(found4);
                assert(found5);
                assert(found6);
                assert(found7);
                assert(found8);

                count1 = count2 = count3 = count4 = count5 = count6 = 0;
                found1 = found2 = found3 = found4 = found5 = found6 = found7 = found8 = false;
            }

            graph.create_edge(h4, graph.flip(h4));

            // DeletableHandleGraph has correct structure after creating a reversing self-loop
            {
                assert(graph.get_edge_count() == 3);

                graph.follow_edges(h4, false, [&](const handle_t& next) {
                    if (next == graph.flip(h4)) {
                        found1 = true;
                    }
                    count1++;
                    return true;
                });
                graph.follow_edges(graph.flip(h4), true, [&](const handle_t& prev) {
                    if (prev == h4) {
                        found2 = true;
                    }
                    count2++;
                    return true;
                });
                assert(count1 == 1);
                assert(count2 == 1);
                assert(found1);
                assert(found2);

                count1 = count2 = 0;
                found1 = found2 = false;
            }

            graph.create_edge(h, graph.flip(h4));
            graph.create_edge(graph.flip(h3), h4);

            assert(graph.get_edge_count() == 5);

            graph.destroy_edge(h, graph.flip(h4));
            graph.destroy_edge(graph.flip(h3), h4);

            assert(graph.get_edge_count() == 3);

            // DeletableHandleGraph has correct structure after creating and deleting edges
            {
                graph.follow_edges(h, false, [&](const handle_t& next) {
                    if (next == h2) {
                        found1 = true;
                    }
                    else if (next == graph.flip(h3)) {
                        found2 = true;
                    }
                    count1++;
                    return true;
                });
                graph.follow_edges(graph.flip(h), true, [&](const handle_t& prev) {
                    if (prev == graph.flip(h2)) {
                        found3 = true;
                    }
                    else if (prev == h3) {
                        found4 = true;
                    }
                    count2++;
                    return true;
                });
                graph.follow_edges(h2, true, [&](const handle_t& prev) {
                    if (prev == h) {
                        found5 = true;
                    }
                    count3++;
                    return true;
                });
                graph.follow_edges(graph.flip(h2), false, [&](const handle_t& next) {
                    if (next == graph.flip(h)) {
                        found6 = true;
                    }
                    count4++;
                    return true;
                });
                graph.follow_edges(graph.flip(h3), true, [&](const handle_t& prev) {
                    if (prev == h) {
                        found7 = true;
                    }
                    count5++;
                    return true;
                });
                graph.follow_edges(h3, false, [&](const handle_t& next) {
                    if (next == graph.flip(h)) {
                        found8 = true;
                    }
                    count6++;
                    return true;
                });
                assert(count1 == 2);
                assert(count2 == 2);
                assert(count3 == 1);
                assert(count4 == 1);
                assert(count5 == 1);
                assert(count6 == 1);
                assert(found1);
                assert(found2);
                assert(found3);
                assert(found4);
                assert(found5);
                assert(found6);
                assert(found7);
                assert(found8);

                count1 = count2 = count3 = count4 = count5 = count6 = 0;
                found1 = found2 = found3 = found4 = found5 = found6 = found7 = found8 = false;

                graph.follow_edges(h4, false, [&](const handle_t& next) {
                    if (next == graph.flip(h4)) {
                        found1 = true;
                    }
                    count1++;
                    return true;
                });
                graph.follow_edges(graph.flip(h4), true, [&](const handle_t& prev) {
                    if (prev == h4) {
                        found2 = true;
                    }
                    count2++;
                    return true;
                });
                assert(count1 == 1);
                assert(count2 == 1);
                assert(found1);
                assert(found2);

                count1 = count2 = 0;
                found1 = found2 = false;
            }

            handle_t h5 = graph.create_handle("GGACC");

            // make some edges to ensure that deleting is difficult
            graph.create_edge(h, h5);
            graph.create_edge(h5, h);
            graph.create_edge(graph.flip(h5), h2);
            graph.create_edge(h3, graph.flip(h5));
            graph.create_edge(h3, h5);
            graph.create_edge(h5, h4);

            graph.destroy_handle(h5);

            // DeletableHandleGraph has correct structure after creating and deleting a node
            {

                graph.follow_edges(h, false, [&](const handle_t& next) {
                    if (next == h2) {
                        found1 = true;
                    }
                    else if (next == graph.flip(h3)) {
                        found2 = true;
                    }
                    count1++;
                    return true;
                });
                graph.follow_edges(graph.flip(h), true, [&](const handle_t& prev) {
                    if (prev == graph.flip(h2)) {
                        found3 = true;
                    }
                    else if (prev == h3) {
                        found4 = true;
                    }
                    count2++;
                    return true;
                });
                graph.follow_edges(h2, true, [&](const handle_t& prev) {
                    if (prev == h) {
                        found5 = true;
                    }
                    count3++;
                    return true;
                });
                graph.follow_edges(graph.flip(h2), false, [&](const handle_t& next) {
                    if (next == graph.flip(h)) {
                        found6 = true;
                    }
                    count4++;
                    return true;
                });
                graph.follow_edges(graph.flip(h3), true, [&](const handle_t& prev) {
                    if (prev == h) {
                        found7 = true;
                    }
                    count5++;
                    return true;
                });
                graph.follow_edges(h3, false, [&](const handle_t& next) {
                    if (next == graph.flip(h)) {
                        found8 = true;
                    }
                    count6++;
                    return true;
                });
                assert(count1 == 2);
                assert(count2 == 2);
                assert(count3 == 1);
                assert(count4 == 1);
                assert(count5 == 1);
                assert(count6 == 1);
                assert(found1);
                assert(found2);
                assert(found3);
                assert(found4);
                assert(found5);
                assert(found6);
                assert(found7);
                assert(found8);

                count1 = count2 = count3 = count4 = count5 = count6 = 0;
                found1 = found2 = found3 = found4 = found5 = found6 = found7 = found8 = false;

                graph.follow_edges(h4, false, [&](const handle_t& next) {
                    if (next == graph.flip(h4)) {
                        found1 = true;
                    }
                    count1++;
                    return true;
                });
                graph.follow_edges(graph.flip(h4), true, [&](const handle_t& prev) {
                    if (prev == h4) {
                        found2 = true;
                    }
                    count2++;
                    return true;
                });
                assert(count1 == 1);
                assert(count2 == 1);
                assert(found1);
                assert(found2);

                count1 = count2 = 0;
                found1 = found2 = false;
            }

            // DeletableHandleGraph has correct structure after swapping nodes
            {

                graph.follow_edges(h, false, [&](const handle_t& next) {
                    if (next == h2) {
                        found1 = true;
                    }
                    else if (next == graph.flip(h3)) {
                        found2 = true;
                    }
                    count1++;
                    return true;
                });
                graph.follow_edges(graph.flip(h), true, [&](const handle_t& prev) {
                    if (prev == graph.flip(h2)) {
                        found3 = true;
                    }
                    else if (prev == h3) {
                        found4 = true;
                    }
                    count2++;
                    return true;
                });
                graph.follow_edges(h2, true, [&](const handle_t& prev) {
                    if (prev == h) {
                        found5 = true;
                    }
                    count3++;
                    return true;
                });
                graph.follow_edges(graph.flip(h2), false, [&](const handle_t& next) {
                    if (next == graph.flip(h)) {
                        found6 = true;
                    }
                    count4++;
                    return true;
                });
                graph.follow_edges(graph.flip(h3), true, [&](const handle_t& prev) {
                    if (prev == h) {
                        found7 = true;
                    }
                    count5++;
                    return true;
                });
                graph.follow_edges(h3, false, [&](const handle_t& next) {
                    if (next == graph.flip(h)) {
                        found8 = true;
                    }
                    count6++;
                    return true;
                });
                assert(count1 == 2);
                assert(count2 == 2);
                assert(count3 == 1);
                assert(count4 == 1);
                assert(count5 == 1);
                assert(count6 == 1);
                assert(found1);
                assert(found2);
                assert(found3);
                assert(found4);
                assert(found5);
                assert(found6);
                assert(found7);
                assert(found8);

                count1 = count2 = count3 = count4 = count5 = count6 = 0;
                found1 = found2 = found3 = found4 = found5 = found6 = found7 = found8 = false;

                graph.follow_edges(h4, false, [&](const handle_t& next) {
                    if (next == graph.flip(h4)) {
                        found1 = true;
                    }
                    count1++;
                    return true;
                });
                graph.follow_edges(graph.flip(h4), true, [&](const handle_t& prev) {
                    if (prev == h4) {
                        found2 = true;
                    }
                    count2++;
                    return true;
                });
                assert(count1 == 1);
                assert(count2 == 1);
                assert(found1);
                assert(found2);

                count1 = count2 = 0;
                found1 = found2 = false;
            }

            // DeletableHandleGraph visits all nodes with for_each_handle
            {
                graph.for_each_handle([&](const handle_t& handle) {
                    if (handle == h) {
                        found1 = true;
                    }
                    else if (handle == h2) {
                        found2 = true;
                    }
                    else if (handle == h3) {
                        found3 = true;
                    }
                    else if (handle == h4) {
                        found4 = true;
                    }
                    else {
                        assert(false);
                    }
                    return true;
                });

                assert(found1);
                assert(found2);
                assert(found3);
                assert(found4);

                found1 = found2 = found3 = found4 = false;
            }

            // to make sure the sequence reverse complemented correctly
            int i = 0;
            auto check_rev_comp = [&](const std::string& seq1, const std::string& seq2) {
                i++;
                assert(seq1.size() == seq2.size());
                auto it = seq1.begin();
                auto rit = seq2.rbegin();
                for (; it != seq1.end(); it++) {
                    if (*it == 'A') {
                        assert(*rit == 'T');
                    }
                    else if (*it == 'C') {
                        assert(*rit == 'G');
                    }
                    else if (*it == 'G') {
                        assert(*rit == 'C');
                    }
                    else if (*it == 'T') {
                        assert(*rit == 'A');
                    }
                    else if (*it == 'N') {
                        assert(*rit == 'N');
                    }
                    else {
                        assert(false);
                    }

                    rit++;
                }
            };


            int count7 = 0, count8 = 0;

            // DeletableHandleGraph correctly reverses a node
            {

                string seq1 = graph.get_sequence(h);
                h = graph.apply_orientation(graph.flip(h));

                // check the sequence
                string rev_seq1 = graph.get_sequence(h);
                check_rev_comp(seq1, rev_seq1);

                // check that the edges are what we expect

                graph.follow_edges(h, false, [&](const handle_t& next) {
                    count1++;
                    return true;
                });
                graph.follow_edges(h, true, [&](const handle_t& prev) {
                    if (prev == graph.flip(h2)) {
                        found1 = true;
                    }
                    else if (prev == h3) {
                        found2 = true;
                    }
                    count2++;
                    return true;
                });
                graph.follow_edges(graph.flip(h), true, [&](const handle_t& next) {
                    count3++;
                    return true;
                });
                graph.follow_edges(graph.flip(h), false, [&](const handle_t& prev) {
                    if (prev == h2) {
                        found3 = true;
                    }
                    else if (prev == graph.flip(h3)) {
                        found4 = true;
                    }
                    count4++;
                    return true;
                });
                graph.follow_edges(h2, true, [&](const handle_t& prev) {
                    if (prev == graph.flip(h)) {
                        found5 = true;
                    }
                    count5++;
                    return true;
                });
                graph.follow_edges(graph.flip(h2), false, [&](const handle_t& next) {
                    if (next == h) {
                        found6 = true;
                    }
                    count6++;
                    return true;
                });
                graph.follow_edges(graph.flip(h3), true, [&](const handle_t& prev) {
                    if (prev == graph.flip(h)) {
                        found7 = true;
                    }
                    count7++;
                    return true;
                });
                graph.follow_edges(h3, false, [&](const handle_t& next) {
                    if (next == h) {
                        found8 = true;
                    }
                    count8++;
                    return true;
                });
                assert(count1 == 0);
                assert(count2 == 2);
                assert(count3 == 0);
                assert(count4 == 2);
                assert(count5 == 1);
                assert(count6 == 1);
                assert(count7 == 1);
                assert(count8 == 1);
                assert(found1);
                assert(found2);
                assert(found3);
                assert(found4);
                assert(found5);
                assert(found6);
                assert(found7);
                assert(found8);

                count1 = count2 = count3 = count4 = count5 = count6 = count7 = count8 = 0;
                found1 = found2 = found3 = found4 = found5 = found6 = found7 = found8 = false;


                // and now switch it back to the same orientation and repeat the topology checks

                h = graph.apply_orientation(graph.flip(h));

                graph.follow_edges(h, false, [&](const handle_t& next) {
                    if (next == h2) {
                        found1 = true;
                    }
                    else if (next == graph.flip(h3)) {
                        found2 = true;
                    }
                    count1++;
                    return true;
                });
                graph.follow_edges(graph.flip(h), true, [&](const handle_t& prev) {
                    if (prev == graph.flip(h2)) {
                        found3 = true;
                    }
                    else if (prev == h3) {
                        found4 = true;
                    }
                    count2++;
                    return true;
                });
                graph.follow_edges(h2, true, [&](const handle_t& prev) {
                    if (prev == h) {
                        found5 = true;
                    }
                    count3++;
                    return true;
                });
                graph.follow_edges(graph.flip(h2), false, [&](const handle_t& next) {
                    if (next == graph.flip(h)) {
                        found6 = true;
                    }
                    count4++;
                    return true;
                });
                graph.follow_edges(graph.flip(h3), true, [&](const handle_t& prev) {
                    if (prev == h) {
                        found7 = true;
                    }
                    count5++;
                    return true;
                });
                graph.follow_edges(h3, false, [&](const handle_t& next) {
                    if (next == graph.flip(h)) {
                        found8 = true;
                    }
                    count6++;
                    return true;
                });
                assert(count1 == 2);
                assert(count2 == 2);
                assert(count3 == 1);
                assert(count4 == 1);
                assert(count5 == 1);
                assert(count6 == 1);
                assert(found1);
                assert(found2);
                assert(found3);
                assert(found4);
                assert(found5);
                assert(found6);
                assert(found7);
                assert(found8);

                count1 = count2 = count3 = count4 = count5 = count6 = 0;
                found1 = found2 = found3 = found4 = found5 = found6 = found7 = found8 = false;

                graph.follow_edges(h4, false, [&](const handle_t& next) {
                    if (next == graph.flip(h4)) {
                        found1 = true;
                    }
                    count1++;
                    return true;
                });
                graph.follow_edges(graph.flip(h4), true, [&](const handle_t& prev) {
                    if (prev == h4) {
                        found2 = true;
                    }
                    count2++;
                    return true;
                });
                assert(count1 == 1);
                assert(count2 == 1);
                assert(found1);
                assert(found2);

                count1 = count2 = 0;
                found1 = found2 = false;
            }

            vector<handle_t> parts = graph.divide_handle(h, vector<size_t>{1, 2});

            int count9 = 0, count10 = 0, count11 = 0, count12 = 0;
            bool found9 = false, found10 = false, found11 = false, found12 = false, found13 = false, found14 = false;

            // DeletableHandleGraph can correctly divide a node
            {

                assert(parts.size() == 3);

                assert(graph.get_sequence(parts[0]) == "A");
                assert(graph.get_length(parts[0]) == 1);
                assert(graph.get_sequence(parts[1]) == "T");
                assert(graph.get_length(parts[1]) == 1);
                assert(graph.get_sequence(parts[2]) == "G");
                assert(graph.get_length(parts[2]) == 1);


                graph.follow_edges(parts[0], false, [&](const handle_t& next) {
                    if (next == parts[1]) {
                        found1 = true;
                    }
                    count1++;
                    return true;
                });
                graph.follow_edges(parts[0], true, [&](const handle_t& prev) {
                    count2++;
                    return true;
                });
                graph.follow_edges(graph.flip(parts[0]), true, [&](const handle_t& prev) {
                    if (prev == graph.flip(parts[1])) {
                        found2 = true;
                    }
                    count3++;
                    return true;
                });
                graph.follow_edges(graph.flip(parts[0]), false, [&](const handle_t& next) {
                    count4++;
                    return true;
                });

                graph.follow_edges(parts[1], false, [&](const handle_t& next) {
                    if (next == parts[2]) {
                        found3 = true;
                    }
                    count5++;
                    return true;
                });
                graph.follow_edges(parts[1], true, [&](const handle_t& prev) {
                    if (prev == parts[0]) {
                        found4 = true;
                    }
                    count6++;
                    return true;
                });
                graph.follow_edges(graph.flip(parts[1]), true, [&](const handle_t& prev) {
                    if (prev == graph.flip(parts[2])) {
                        found5 = true;
                    }
                    count7++;
                    return true;
                });
                graph.follow_edges(graph.flip(parts[1]), false, [&](const handle_t& next) {
                    if (next == graph.flip(parts[0])) {
                        found6 = true;
                    }
                    count8++;
                    return true;
                });

                graph.follow_edges(parts[2], false, [&](const handle_t& next) {
                    if (next == h2) {
                        found7 = true;
                    }
                    else if (next == graph.flip(h3)) {
                        found8 = true;
                    }
                    count9++;
                    return true;
                });
                graph.follow_edges(parts[2], true, [&](const handle_t& prev) {
                    if (prev == parts[1]) {
                        found9 = true;
                    }
                    count10++;
                    return true;
                });
                graph.follow_edges(graph.flip(parts[2]), true, [&](const handle_t& prev) {
                    if (prev == graph.flip(h2)) {
                        found10 = true;
                    }
                    else if (prev == h3) {
                        found11 = true;
                    }
                    count11++;
                    return true;
                });
                graph.follow_edges(graph.flip(parts[2]), false, [&](const handle_t& next) {
                    if (next == graph.flip(parts[1])) {
                        found12 = true;
                    }
                    count12++;
                    return true;
                });
                graph.follow_edges(graph.flip(h3), true, [&](const handle_t& prev) {
                    if (prev == parts[2]) {
                        found13 = true;
                    }
                    return true;
                });
                graph.follow_edges(h2, true, [&](const handle_t& prev) {
                    if (prev == parts[2]) {
                        found14 = true;
                    }
                    return true;
                });

                assert(count1 == 1);
                assert(count2 == 0);
                assert(count3 == 1);
                assert(count4 == 0);
                assert(count5 == 1);
                assert(count6 == 1);
                assert(count7 == 1);
                assert(count8 == 1);
                assert(count9 == 2);
                assert(count10 == 1);
                assert(count11 == 2);
                assert(count12 == 1);
                assert(found1);
                assert(found2);
                assert(found3);
                assert(found4);
                assert(found5);
                assert(found6);
                assert(found7);
                assert(found8);
                assert(found9);
                assert(found10);
                assert(found11);
                assert(found12);
                assert(found13);
                assert(found14);

                count1 = count2 = count3 = count4 = count5 = count6 = count7 = count8 = count9 = count10 = count11 = count12 = 0;
                found1 = found2 = found3 = found4 = found5 = found6 = found7 = found8 = found9 = found10 = found11 = found12 = false;
            }

            vector<handle_t> rev_parts = graph.divide_handle(graph.flip(h3), vector<size_t>{1});

            // DeletableHandleGraph can correctly divide a node on the reverse strand
            {

                assert(graph.get_sequence(rev_parts[0]) == "G");
                assert(graph.get_length(rev_parts[0]) == 1);
                assert(graph.get_is_reverse(rev_parts[0]));
                assert(graph.get_sequence(rev_parts[1]) == "TC");
                assert(graph.get_length(rev_parts[1]) == 2);
                assert(graph.get_is_reverse(rev_parts[1]));

                graph.follow_edges(rev_parts[0], false, [&](const handle_t& next) {
                    if (next == rev_parts[1]) {
                        found1 = true;
                    }
                    count1++;
                    return true;
                });
                graph.follow_edges(rev_parts[1], true, [&](const handle_t& prev) {
                    if (prev == rev_parts[0]) {
                        found2 = true;
                    }
                    count2++;
                    return true;
                });
                graph.follow_edges(graph.flip(rev_parts[1]), false, [&](const handle_t& next) {
                    if (next == graph.flip(rev_parts[0])) {
                        found3 = true;
                    }
                    count3++;
                    return true;
                });
                graph.follow_edges(graph.flip(rev_parts[0]), true, [&](const handle_t& prev) {
                    if (prev == graph.flip(rev_parts[1])) {
                        found4 = true;
                    }
                    count4++;
                    return true;
                });
                graph.follow_edges(rev_parts[0], true, [&](const handle_t& prev) {
                    if (prev == parts[2]) {
                        found5 = true;
                    }
                    count5++;
                    return true;
                });
                graph.follow_edges(rev_parts[1], false, [&](const handle_t& next) {
                    count6++;
                    return true;
                });

                assert(count1 == 1);
                assert(count2 == 1);
                assert(count3 == 1);
                assert(count4 == 1);
                assert(count5 == 1);
                assert(count6 == 0);
                assert(found1);
                assert(found2);
                assert(found3);
                assert(found4);
                assert(found5);
            }

            auto h6 = graph.create_handle("ACGT");
            auto h7 = graph.create_handle("GCGG");
            auto h8 = graph.create_handle("TTCA");

            graph.create_edge(h6, h7);
            graph.create_edge(h7, h8);

            h7 = graph.truncate_handle(h7, true, 1);
            assert(graph.get_sequence(h7) == "CGG");
            assert(graph.get_degree(h7, true) == 0);
            assert(graph.get_degree(h7, false) == 1);
            assert(graph.get_degree(h6, false) == 0);
            assert(graph.get_degree(h8, true) == 1);

            h7 = graph.truncate_handle(h7, false, 2);
            assert(graph.get_sequence(h7) == "CG");
            assert(graph.get_degree(h7, true) == 0);
            assert(graph.get_degree(h7, false) == 0);
            assert(graph.get_degree(h6, false) == 0);
            assert(graph.get_degree(h8, true) == 0);
        }
    }

    // second batch of test involving self loops
    {
        vector<DeletableHandleGraph*> implementations;

        PackedGraph pg;
        implementations.push_back(&pg);

        HashGraph hg;
        implementations.push_back(&hg);

        ODGI og;
        implementations.push_back(&og);

        for (DeletableHandleGraph* implementation : implementations) {

            DeletableHandleGraph& graph = *implementation;

            // initialize the graph

            handle_t h1 = graph.create_handle("A");
            handle_t h2 = graph.create_handle("C");

            graph.create_edge(h1, h2);
            graph.create_edge(graph.flip(h1), h2);

            // test for the right initial topology
            bool found1 = false, found2 = false, found3 = false, found4 = false, found5 = false, found6 = false;
            int count1 = 0, count2 = 0, count3 = 0, count4 = 0;

            graph.follow_edges(h1, false, [&](const handle_t& other) {
                if (other == h2) {
                    found1 = true;
                }
                count1++;
            });
            graph.follow_edges(h1, true, [&](const handle_t& other) {
                if (other == graph.flip(h2)) {
                    found2 = true;
                }
                count2++;
            });
            graph.follow_edges(h2, false, [&](const handle_t& other) {
                count3++;
            });
            graph.follow_edges(h2, true, [&](const handle_t& other) {
                if (other == h1) {
                    found3 = true;
                }
                else if (other == graph.flip(h1)) {
                    found4 = true;
                }
                count4++;
            });
            assert(found1);
            assert(found2);
            assert(found3);
            assert(found4);
            assert(count1 == 1);
            assert(count2 == 1);
            assert(count3 == 0);
            assert(count4 == 2);
            found1 = found2 = found3 = found4 = found5 = found6 = false;
            count1 = count2 = count3 = count4 = 0;

            // flip a node and check if the orientation is correct
            h1 = graph.apply_orientation(graph.flip(h1));

            graph.follow_edges(h1, false, [&](const handle_t& other) {
                if (other == h2) {
                    found1 = true;
                }
                count1++;
            });
            graph.follow_edges(h1, true, [&](const handle_t& other) {
                if (other == graph.flip(h2)) {
                    found2 = true;
                }
                count2++;
            });
            graph.follow_edges(h2, false, [&](const handle_t& other) {
                count3++;
            });
            graph.follow_edges(h2, true, [&](const handle_t& other) {
                if (other == h1) {
                    found3 = true;
                }
                else if (other == graph.flip(h1)) {
                    found4 = true;
                }
                count4++;
            });
            assert(found1);
            assert(found2);
            assert(found3);
            assert(found4);
            assert(count1 == 1);
            assert(count2 == 1);
            assert(count3 == 0);
            assert(count4 == 2);
            found1 = found2 = found3 = found4 = found5 = found6 = false;
            count1 = count2 = count3 = count4 = 0;

            // create a new edge

            graph.create_edge(h1, graph.flip(h2));

            // check the topology

            graph.follow_edges(h1, false, [&](const handle_t& other) {
                if (other == h2) {
                    found1 = true;
                }
                else if (other == graph.flip(h2)) {
                    found2 = true;
                }
                count1++;
            });
            graph.follow_edges(h1, true, [&](const handle_t& other) {
                if (other == graph.flip(h2)) {
                    found3 = true;
                }
                count2++;
            });
            graph.follow_edges(h2, false, [&](const handle_t& other) {
                if (other == graph.flip(h1)) {
                    found4 = true;
                }
                count3++;
            });
            graph.follow_edges(h2, true, [&](const handle_t& other) {
                if (other == h1) {
                    found5 = true;
                }
                else if (other == graph.flip(h1)) {
                    found6 = true;
                }
                count4++;
            });
            assert(found1);
            assert(found2);
            assert(found3);
            assert(found4);
            assert(found5);
            assert(found6);
            assert(count1 == 2);
            assert(count2 == 1);
            assert(count3 == 1);
            assert(count4 == 2);
            found1 = found2 = found3 = found4 = found5 = found6 = false;
            count1 = count2 = count3 = count4 = 0;

            // now another node and check to make sure that the edges are updated appropriately

            h2 = graph.apply_orientation(graph.flip(h2));

            graph.follow_edges(h1, false, [&](const handle_t& other) {
                if (other == h2) {
                    found1 = true;
                }
                else if (other == graph.flip(h2)) {
                    found2 = true;
                }
                count1++;
            });
            graph.follow_edges(h1, true, [&](const handle_t& other) {
                if (other == h2) {
                    found3 = true;
                }
                count2++;
            });
            graph.follow_edges(h2, false, [&](const handle_t& other) {
                if (other == h1) {
                    found4 = true;
                }
                else if (other == graph.flip(h1)) {
                    found5 = true;
                }
                count3++;
            });
            graph.follow_edges(h2, true, [&](const handle_t& other) {
                if (other == h1) {
                    found6 = true;
                }
                count4++;
            });
            assert(found1);
            assert(found2);
            assert(found3);
            assert(found4);
            assert(found5);
            assert(found6);
            assert(count1 == 2);
            assert(count2 == 1);
            assert(count3 == 2);
            assert(count4 == 1);
        }
    }

    // another batch of tests involving divide handle and reversing
    // self edges
    {
        vector<DeletableHandleGraph*> implementations;

        HashGraph hg;
        implementations.push_back(&hg);

        PackedGraph pg;
        implementations.push_back(&pg);

        //ODGI og;
        //implementations.push_back(&og);

        for (DeletableHandleGraph* implementation : implementations) {
            DeletableHandleGraph& graph = *implementation;

            handle_t h1 = graph.create_handle("ATGAA");
            handle_t h2 = graph.create_handle("ATGAA");

            graph.create_edge(h1, graph.flip(h1));
            graph.create_edge(graph.flip(h2), h2);

            auto parts1 = graph.divide_handle(h1, {2, 4});
            auto parts2 = graph.divide_handle(h2, {2, 4});

            assert(parts1.size() == 3);
            assert(parts2.size() == 3);

            assert(graph.has_edge(parts1[0], parts1[1]));
            assert(graph.has_edge(parts1[1], parts1[2]));
            assert(graph.has_edge(parts1[2], graph.flip(parts1[2])));

            assert(graph.has_edge(parts2[0], parts2[1]));
            assert(graph.has_edge(parts2[1], parts2[2]));
            assert(graph.has_edge(graph.flip(parts2[0]), parts2[0]));

        }
    }
    
    // another batch of tests that deal with deleting after dividing
    {
        vector<pair<MutablePathDeletableHandleGraph*, MutablePathDeletableHandleGraph*>> implementations;
        
        // Add implementations
        
        PackedGraph pg, pg2;
        implementations.push_back(make_pair(&pg, &pg2));
        
        ODGI og, og2;
        implementations.push_back(make_pair(&og, &og2));

        HashGraph hg, hg2;
        implementations.push_back(make_pair(&hg, &hg2));
        
        // And test them
        for (int imp = 0; imp < implementations.size(); ++imp) {
            
            for (bool backwards : {false, true}) {
                
                MutablePathDeletableHandleGraph* g = backwards ? implementations[imp].first : implementations[imp].second;
                
                assert(g->get_node_count() == 0);
                
                handle_t handle1 = g->create_handle("CAAATAAGGCTTGGAAATTTTCTGGAGTTCTA");
                handle_t handle2 = g->create_handle("TTATATTCCAACTCTCTG");
                path_handle_t path_handle = g->create_path_handle("x");
                g->create_edge(handle1, handle2);
                
                if (backwards) {
                    handle1 = g->flip(handle1);
                    handle2 = g->flip(handle2);
                    g->append_step(path_handle, handle2);
                    g->append_step(path_handle, handle1);
                } else {
                    g->append_step(path_handle, handle1);
                    g->append_step(path_handle, handle2);
                }
                                
                auto parts1 = g->divide_handle(handle1, vector<size_t>({2, 7, 22, 31}));
                auto parts2 = g->divide_handle(handle2, vector<size_t>({1, 5, 10}));
                                
                vector<handle_t> steps;
                g->for_each_step_in_path(path_handle, [&](step_handle_t step_handle) {
                    steps.push_back(g->get_handle_of_step(step_handle));
                });
                                
                assert(steps.size() == 9);
                int i = 0;
                vector<handle_t> to_delete;
                g->append_step(g->create_path_handle(to_string(i)), steps[i]);
                ++i;
                to_delete.push_back(steps[i++]);
                g->append_step(g->create_path_handle(to_string(i)), steps[i]);
                ++i;
                to_delete.push_back(steps[i++]);
                to_delete.push_back(steps[i++]);
                to_delete.push_back(steps[i++]);
                g->append_step(g->create_path_handle(to_string(i)), steps[i]);
                ++i;
                to_delete.push_back(steps[i++]);
                g->append_step(g->create_path_handle(to_string(i)), steps[i]);
                ++i;
                
                g->destroy_path(path_handle);
                                
                for (auto handle : to_delete) {
                    g->destroy_handle(handle);
                }
                
                g->for_each_path_handle([&](const path_handle_t& p) {
                    g->for_each_step_in_path(p, [&](const step_handle_t& s) {
                        auto h = g->get_handle_of_step(s);
                    });
                });
                
                assert(g->get_node_count() == 4);
                assert(g->get_path_count() == 4);
            }
        }
    }
    
    cerr << "DeletableHandleGraph tests successful!" << endl;
}

void test_mutable_path_handle_graphs() {
    
    vector<MutablePathDeletableHandleGraph*> implementations;
    
    PackedGraph pg;
    implementations.push_back(&pg);
    
    HashGraph hg;
    implementations.push_back(&hg);

    ODGI og;
    implementations.push_back(&og);
    
    for (MutablePathDeletableHandleGraph* implementation : implementations) {

        auto check_path = [&](MutablePathDeletableHandleGraph& graph, const path_handle_t& p, const vector<handle_t>& steps) {
            assert(graph.get_step_count(p) == steps.size());

            // Make sure steps connect back to the path
            step_handle_t begin_step = graph.path_begin(p);
            step_handle_t end_step = graph.path_end(p);
            assert(graph.get_path_handle_of_step(begin_step) == p);
            assert(graph.get_path_handle_of_step(end_step) == p);

            step_handle_t step = graph.path_begin(p);
            for (int i = 0; i < steps.size(); i++) {
                auto here = graph.get_handle_of_step(step);
                assert(graph.get_path_handle_of_step(step) == p);
                assert(graph.get_handle_of_step(step) == steps[i]);

                if (graph.get_is_circular(p)) {
                    assert(graph.has_next_step(step));
                    assert(graph.has_previous_step(step));
                }
                else {
                    assert(graph.has_next_step(step) == i + 1 < steps.size());
                    assert(graph.has_previous_step(step) == i > 0);
                }

                step = graph.get_next_step(step);
            }

            if (graph.get_is_circular(p) && !graph.is_empty(p)) {
                assert(step == graph.path_begin(p));
            }
            else {
                assert(step == graph.path_end(p));
            }

            step = graph.path_back(p);

            for (int i = steps.size() - 1; i >= 0; i--) {

                assert(graph.get_path_handle_of_step(step) == p);
                assert(graph.get_handle_of_step(step) == steps[i]);

                if (graph.get_is_circular(p)) {
                    assert(graph.has_next_step(step));
                    assert(graph.has_previous_step(step));
                }
                else {
                    assert(graph.has_next_step(step) == i + 1 < steps.size());
                    assert(graph.has_previous_step(step) == i > 0);
                }

                step = graph.get_previous_step(step);
            }

            if (graph.get_is_circular(p) && !graph.is_empty(p)) {
                assert(step == graph.path_back(p));
            }
            else {
                assert(step == graph.path_front_end(p));
            }
        };

        auto check_flips = [&](MutablePathDeletableHandleGraph& graph, const path_handle_t& p, const vector<handle_t>& steps) {

            auto flipped = steps;
            for (size_t i = 0; i < steps.size(); i++) {
                graph.apply_orientation(graph.flip(graph.forward(flipped[i])));
                flipped[i] = graph.flip(flipped[i]);
                check_path(graph, p, flipped);

                graph.apply_orientation(graph.flip(graph.forward(flipped[i])));
                flipped[i] = graph.flip(flipped[i]);
                check_path(graph, p, flipped);
            }
        };

        MutablePathDeletableHandleGraph& graph = *implementation;

        handle_t h1 = graph.create_handle("AC");
        handle_t h2 = graph.create_handle("CAGTGA");
        handle_t h3 = graph.create_handle("GT");

        graph.create_edge(h1, h2);
        graph.create_edge(h2, h3);
        graph.create_edge(h1, graph.flip(h2));
        graph.create_edge(graph.flip(h2), h3);

        assert(!graph.has_path("1"));
        assert(graph.get_path_count() == 0);

        path_handle_t p1 = graph.create_path_handle("1");

        assert(graph.has_path("1"));
        assert(graph.get_path_count() == 1);
        assert(graph.get_path_handle("1") == p1);
        assert(graph.get_path_name(p1) == "1");
        assert(graph.get_step_count(p1) == 0);
        assert(graph.is_empty(p1));

        graph.append_step(p1, h1);

        assert(graph.get_step_count(p1) == 1);
        assert(!graph.is_empty(p1));

        graph.append_step(p1, h2);
        graph.append_step(p1, h3);

        assert(graph.get_step_count(p1) == 3);

        // graph can traverse a path
        check_path(graph, p1, {h1, h2, h3});

        // graph preserves paths when reversing nodes
        check_flips(graph, p1, {h1, h2, h3});

        // make a circular path
        path_handle_t p2 = graph.create_path_handle("2", true);
        assert(graph.get_path_count() == 2);

        graph.append_step(p2, h1);
        graph.append_step(p2, graph.flip(h2));
        graph.append_step(p2, h3);

        check_path(graph, p2, {h1, graph.flip(h2), h3});

        // graph can query steps of a node on paths

        bool found1 = false, found2 = false;
        vector<step_handle_t> steps = graph.steps_of_handle(h1);
        for (auto& step : steps) {
            if (graph.get_path_handle_of_step(step) == p1 &&
                graph.get_handle_of_step(step) == h1) {
                found1 = true;
            }
            else if (graph.get_path_handle_of_step(step) == p2 &&
                     graph.get_handle_of_step(step) == h1) {
                found2 = true;
            }
            else {
                assert(false);
            }
        }
        assert(found1);
        assert(found2);
        found1 = found2 = false;

        steps = graph.steps_of_handle(h1, true);
        for (auto& step : steps) {
            if (graph.get_path_handle_of_step(step) == p1 &&
                graph.get_handle_of_step(step) == h1) {
                found1 = true;
            }
            else if (graph.get_path_handle_of_step(step) == p2 &&
                     graph.get_handle_of_step(step) == h1) {
                found2 = true;
            }
            else {
                assert(false);
            }
        }
        assert(found1);
        assert(found2);
        found1 = found2 = false;

        steps = graph.steps_of_handle(graph.flip(h1), true);
        for (auto& step : steps) {
            assert(false);
        }

        steps = graph.steps_of_handle(h2, true);
        for (auto& step : steps) {
            if (graph.get_path_handle_of_step(step) == p1 &&
                graph.get_handle_of_step(step) == h2) {
                found1 = true;
            }
            else {
                assert(false);
            }
        }
        steps = graph.steps_of_handle(graph.flip(h2), true);
        for (auto& step : steps) {
            if (graph.get_path_handle_of_step(step) == p2 &&
                graph.get_handle_of_step(step) == graph.flip(h2)) {
                found2 = true;
            }
            else {
                assert(false);
            }
        }
        assert(found1);
        assert(found2);
        found1 = found2 = false;

        vector<handle_t> segments = graph.divide_handle(h2, {size_t(2), size_t(4)});

        // graph preserves paths when dividing nodes

        check_path(graph, p1, {h1, segments[0], segments[1], segments[2], h3});
        check_path(graph, p2, {h1, graph.flip(segments[2]), graph.flip(segments[1]), graph.flip(segments[0]), h3});

        path_handle_t p3 = graph.create_path_handle("3");
        graph.append_step(p3, h1);
        graph.append_step(p3, segments[0]);

        assert(graph.has_path("3"));
        assert(graph.get_path_count() == 3);

        // graph can toggle circularity

        graph.for_each_path_handle([&](const path_handle_t& p) {

            vector<handle_t> steps;

            for (handle_t h : graph.scan_path(p)) {
                steps.push_back(h);
            }

            bool starting_circularity = graph.get_is_circular(p);

            // make every transition occur
            for (bool circularity : {true, true, false, false, true}) {
                graph.set_circularity(p, circularity);
                assert(graph.get_is_circular(p) == circularity);
                check_path(graph, p, steps);
            }

            graph.set_circularity(p, starting_circularity);
        });

        // graph can destroy paths

        graph.destroy_path(p3);

        assert(!graph.has_path("3"));
        assert(graph.get_path_count() == 2);

        bool found3 = false;

        graph.for_each_path_handle([&](const path_handle_t& p) {
            if (graph.get_path_name(p) == "1") {
                found1 = true;
            }
            else if (graph.get_path_name(p) == "2") {
                found2 = true;
            }
            else if (graph.get_path_name(p) == "3") {
                found3 = true;
            }
            else {
                assert(false);
            }
        });

        assert(found1);
        assert(found2);
        assert(!found3);

        // check flips to see if membership records are still functional
        check_flips(graph, p1, {h1, segments[0], segments[1], segments[2], h3});
        check_flips(graph, p2, {h1, graph.flip(segments[2]), graph.flip(segments[1]), graph.flip(segments[0]), h3});

        graph.destroy_path(p1);

        assert(!graph.has_path("1"));
        assert(graph.get_path_count() == 1);

        found1 = found2 = found3 = false;

        graph.for_each_path_handle([&](const path_handle_t& p) {
            if (graph.get_path_name(p) == "1") {
                found1 = true;
            }
            else if (graph.get_path_name(p) == "2") {
                found2 = true;
            }
            else if (graph.get_path_name(p) == "3") {
                found3 = true;
            }
            else {
                assert(false);
            }
        });

        assert(!found1);
        assert(found2);
        assert(!found3);

        // check flips to see if membership records are still functional
        check_flips(graph, p2, {h1, graph.flip(segments[2]), graph.flip(segments[1]), graph.flip(segments[0]), h3});

        // make a path to rewrite
        path_handle_t p4 = graph.create_path_handle("4");
        graph.prepend_step(p4, h3);
        graph.prepend_step(p4, segments[2]);
        graph.prepend_step(p4, segments[1]);
        graph.prepend_step(p4, segments[0]);
        graph.prepend_step(p4, h1);

        check_flips(graph, p4, {h1, segments[0], segments[1], segments[2], h3});

        auto check_rewritten_segment = [&](const pair<step_handle_t, step_handle_t>& new_segment,
                                           const vector<handle_t>& steps) {
            int i = 0;
            for (auto step = new_segment.first; step != new_segment.second; step = graph.get_next_step(step)) {
                assert(graph.get_handle_of_step(step) == steps[i]);
                i++;
            }
            assert(i == steps.size());
        };

        // rewrite the middle portion of a path

        step_handle_t s1 = graph.get_next_step(graph.path_begin(p4));
        step_handle_t s2 = graph.get_next_step(graph.get_next_step(graph.get_next_step(s1)));

        auto new_segment = graph.rewrite_segment(s1, s2, {graph.flip(segments[2]), graph.flip(segments[1]), graph.flip(segments[0])});

        check_flips(graph, p4, {h1, graph.flip(segments[2]), graph.flip(segments[1]), graph.flip(segments[0]), h3});
        check_rewritten_segment(new_segment, {graph.flip(segments[2]), graph.flip(segments[1]), graph.flip(segments[0])});

        // rewrite around the end of a circular path to delete

        graph.create_edge(h3, h1);
        graph.create_edge(segments[2], segments[0]);
        graph.set_circularity(p4, true);

        s1 = graph.get_previous_step(graph.path_begin(p4));
        s2 = graph.get_next_step(graph.path_begin(p4));
        assert(s2 != graph.path_end(p4));

        new_segment = graph.rewrite_segment(s1, s2, vector<handle_t>());
        // The end we get should be the same as the end we sent, since it is exclusive
        assert(new_segment.second == s2);

        check_flips(graph, p4, {graph.flip(segments[2]), graph.flip(segments[1]), graph.flip(segments[0])});
        check_rewritten_segment(new_segment, vector<handle_t>());

        // add into an empty slot

        new_segment = graph.rewrite_segment(new_segment.first, new_segment.second, {graph.flip(h1), graph.flip(h3)});

        check_flips(graph, p4, {graph.flip(h1), graph.flip(h3), graph.flip(segments[2]), graph.flip(segments[1]), graph.flip(segments[0])});
        check_rewritten_segment(new_segment, {graph.flip(h1), graph.flip(h3)});

    }
    
    {
        vector<pair<MutablePathMutableHandleGraph*, MutablePathMutableHandleGraph*>> implementations;
        
        // Add implementations
        
        ODGI og, og2;
        implementations.push_back(make_pair(&og, &og2));
        
        HashGraph hg, hg2;
        implementations.push_back(make_pair(&hg, &hg2));
        
        PackedGraph pg, pg2;
        implementations.push_back(make_pair(&pg, &pg2));
        
        // And test them
        for (int imp = 0; imp < implementations.size(); ++imp) {
            for (bool backwards : {false, true}) {
                
                MutablePathMutableHandleGraph* g = backwards ? implementations[imp].first : implementations[imp].second;
                
                assert(g->get_node_count() == 0);
                
                handle_t handle = g->create_handle("TTATATTCCAACTCTCTG");
                if (backwards) {
                    handle = g->flip(handle);
                }
                path_handle_t path_handle = g->create_path_handle("Path");
                g->append_step(path_handle, handle);
                string seq = g->get_sequence(handle);
                vector<string> true_parts = { seq.substr(0, 1), seq.substr(1, 4), seq.substr(5, 5), seq.substr(10) };
                
                // Should get (C,AGAG,AGTTG,GAATATAA)  (forward)
                // Should get (T,TATA,TTCCA,ACTCTCTG)  (reverse)
                auto parts = g->divide_handle(handle, {1, 5, 10});
                assert(parts.size() == true_parts.size());
                for (int i = 0; i < parts.size(); ++i) {
                    assert(g->get_sequence(parts[i]) == true_parts[i]);
                    assert(g->get_is_reverse(parts[i]) == backwards);
                }
                
                vector<handle_t> steps;
                g->for_each_step_in_path(path_handle, [&](step_handle_t step_handle) {
                    steps.push_back(g->get_handle_of_step(step_handle));
                });
                assert(steps.size() == true_parts.size());
                for (int i = 0; i < parts.size(); ++i) {
                    assert(g->get_sequence(steps[i]) == true_parts[i]);
                    assert(g->get_is_reverse(steps[i]) == backwards);
                }
            }
        }

    }
    
    cerr << "MutablePathDeletableHandleGraph tests successful!" << endl;
}

template<typename PackedVectorImpl>
void test_packed_vector() {
    enum vec_op_t {SET = 0, GET = 1, APPEND = 2, POP = 3, SERIALIZE = 4};
    
    random_device rd;
    default_random_engine prng(rd());
    uniform_int_distribution<int> op_distr(0, 4);
    
    int num_runs = 1000;
    int num_ops = 200;
    int gets_per_op = 5;
    int sets_per_op = 5;
    int appends_per_op = 3;
    int pops_per_op = 1;
    
    for (size_t i = 0; i < num_runs; i++) {
        
        uint64_t next_val = 0;
        
        vector<uint64_t> std_vec;
        PackedVectorImpl dyn_vec;
        
        for (size_t j = 0; j < num_ops; j++) {
            
            vec_op_t op = (vec_op_t) op_distr(prng);
            switch (op) {
                case SET:
                    if (!std_vec.empty()) {
                        for (size_t k = 0; k < sets_per_op; k++) {
                            size_t idx = prng() % dyn_vec.size();
                            std_vec[idx] = next_val;
                            dyn_vec.set(idx, next_val);
                            next_val++;
                        }
                    }
                    
                    break;
                    
                case GET:
                    if (!std_vec.empty()) {
                        for (size_t k = 0; k < gets_per_op; k++) {
                            size_t idx = prng() % dyn_vec.size();
                            assert(std_vec[idx] == dyn_vec.get(idx));
                            next_val++;
                        }
                    }
                    
                    break;
                    
                case APPEND:
                    for (size_t k = 0; k < appends_per_op; k++) {
                        std_vec.push_back(next_val);
                        dyn_vec.append(next_val);
                        next_val++;
                    }
                    
                    break;
                    
                case POP:
                    if (!std_vec.empty()) {
                        for (size_t k = 0; k < pops_per_op; k++) {
                            std_vec.pop_back();
                            dyn_vec.pop();
                        }
                    }
                    
                    break;
                    
                case SERIALIZE:
                {
                    stringstream strm;
                    
                    dyn_vec.serialize(strm);
                    strm.seekg(0);
                    PackedVectorImpl copy_vec(strm);
                    
                    assert(copy_vec.size() == dyn_vec.size());
                    for (size_t i = 0; i < copy_vec.size(); i++) {
                        assert(copy_vec.get(i) == dyn_vec.get(i));
                    }
                    break;
                }
                    
                default:
                    break;
            }
            
            assert(std_vec.empty() == dyn_vec.empty());
            assert(std_vec.size() == dyn_vec.size());
        }
    }
    cerr << "PackedVector (" << typeid(PackedVectorImpl).name() << ") tests successful!" << endl;
}

template<typename PagedVectorImpl>
void test_paged_vector() {
    enum vec_op_t {SET = 0, GET = 1, APPEND = 2, POP = 3, SERIALIZE = 4};
    std::random_device rd;
    std::default_random_engine prng(rd());
    std::uniform_int_distribution<int> op_distr(0, 4);
    std::uniform_int_distribution<int> val_distr(0, 100);
    
    int num_runs = 200;
    int num_ops = 200;
    int gets_per_op = 5;
    int sets_per_op = 5;
    int appends_per_op = 3;
    int pops_per_op = 1;
    
    for (size_t i = 0; i < num_runs; i++) {
        
        uint64_t next_val = val_distr(prng);
        
        std::vector<uint64_t> std_vec;
        PagedVectorImpl dyn_vec;
        
        for (size_t j = 0; j < num_ops; j++) {
            
            vec_op_t op = (vec_op_t) op_distr(prng);
            switch (op) {
                case SET:
                    if (!std_vec.empty()) {
                        for (size_t k = 0; k < sets_per_op; k++) {
                            size_t idx = prng() % dyn_vec.size();
                            std_vec[idx] = next_val;
                            dyn_vec.set(idx, next_val);
                            next_val = val_distr(prng);
                        }
                    }
                    
                    break;
                    
                case GET:
                    if (!std_vec.empty()) {
                        for (size_t k = 0; k < gets_per_op; k++) {
                            size_t idx = prng() % dyn_vec.size();
                            assert(std_vec[idx] == dyn_vec.get(idx));
                            next_val = val_distr(prng);
                        }
                    }
                    
                    break;
                    
                case APPEND:
                    for (size_t k = 0; k < appends_per_op; k++) {
                        std_vec.push_back(next_val);
                        dyn_vec.append(next_val);
                        next_val = val_distr(prng);
                    }
                    
                    break;
                    
                case POP:
                    if (!std_vec.empty()) {
                        for (size_t k = 0; k < pops_per_op; k++) {
                            std_vec.pop_back();
                            dyn_vec.pop();
                        }
                    }
                    
                    break;
                    
                case SERIALIZE:
                {
                    stringstream strm;
                    
                    dyn_vec.serialize(strm);
                    strm.seekg(0);
                    PagedVectorImpl copy_vec(strm);
                    
                    assert(copy_vec.size() == dyn_vec.size());
                    for (size_t i = 0; i < copy_vec.size(); i++) {
                        assert(copy_vec.get(i) == dyn_vec.get(i));
                    }
                    break;
                }
                    
                default:
                    break;
            }
            
            assert(std_vec.empty() == dyn_vec.empty());
            assert(std_vec.size() == dyn_vec.size());
        }
    }
    cerr << "PagedVector (" << typeid(PagedVectorImpl).name() << ") tests successful!" << endl;
}

void test_packed_deque() {
    enum deque_op_t {SET = 0, GET = 1, APPEND_LEFT = 2, POP_LEFT = 3, APPEND_RIGHT = 4, POP_RIGHT = 5, SERIALIZE = 6};
    std::random_device rd;
    std::default_random_engine prng(rd());
    std::uniform_int_distribution<int> op_distr(0, 6);
    
    int num_runs = 1000;
    int num_ops = 200;
    int gets_per_op = 5;
    int sets_per_op = 5;
    int appends_per_op = 3;
    int pops_per_op = 1;
    
    for (size_t i = 0; i < num_runs; i++) {
        
        uint64_t next_val = 0;
        
        std::deque<uint64_t> std_deq;
        PackedDeque<> suc_deq;
        
        for (size_t j = 0; j < num_ops; j++) {
            
            deque_op_t op = (deque_op_t) op_distr(prng);
            switch (op) {
                case SET:
                    if (!std_deq.empty()) {
                        for (size_t k = 0; k < sets_per_op; k++) {
                            size_t idx = prng() % std_deq.size();
                            std_deq[idx] = next_val;
                            suc_deq.set(idx, next_val);
                            next_val++;
                        }
                    }
                    
                    break;
                    
                case GET:
                    if (!std_deq.empty()) {
                        for (size_t k = 0; k < gets_per_op; k++) {
                            size_t idx = prng() % std_deq.size();
                            assert(std_deq[idx] == suc_deq.get(idx));
                            next_val++;
                        }
                    }
                    
                    break;
                    
                case APPEND_LEFT:
                    for (size_t k = 0; k < appends_per_op; k++) {
                        std_deq.push_front(next_val);
                        suc_deq.append_front(next_val);
                        next_val++;
                    }
                    
                    break;
                    
                case POP_LEFT:
                    for (size_t k = 0; k < pops_per_op && !std_deq.empty(); k++) {
                        std_deq.pop_front();
                        suc_deq.pop_front();
                    }
                    
                    break;
                    
                case APPEND_RIGHT:
                    for (size_t k = 0; k < appends_per_op; k++) {
                        std_deq.push_back(next_val);
                        suc_deq.append_back(next_val);
                        next_val++;
                    }
                    
                    break;
                    
                case POP_RIGHT:
                    for (size_t k = 0; k < pops_per_op && !std_deq.empty(); k++) {
                        std_deq.pop_back();
                        suc_deq.pop_back();
                    }
                    
                    break;
                    
                case SERIALIZE:
                {
                    stringstream strm;
                    
                    suc_deq.serialize(strm);
                    strm.seekg(0);
                    PackedDeque<> copy_deq(strm);
                    
                    assert(copy_deq.size() == suc_deq.size());
                    for (size_t i = 0; i < copy_deq.size(); i++) {
                        assert(copy_deq.get(i) == suc_deq.get(i));
                    }
                    break;
                }
                    
                default:
                    break;
            }
            
            assert(std_deq.empty() == suc_deq.empty());
            assert(std_deq.size() == suc_deq.size());
        }
    }
    cerr << "PackedDeque tests successful!" << endl;
}

void test_packed_set() {
    enum set_op_t {INSERT = 0, REMOVE = 1, FIND = 2};
    
    random_device rd;
    default_random_engine prng(rd());
    uniform_int_distribution<int> op_distr(0, 2);
    
    int num_runs = 1000;
    int num_ops = 200;
    int inserts_per_op = 2;
    int prev_inserts_per_op = 1;
    int removes_per_op = 1;
    int finds_per_op = 5;
    
    for (size_t i = 0; i < num_runs; i++) {
        uint64_t next_val = 0;
        
        unordered_set<uint64_t> std_set;
        PackedSet<> packed_set;
        
        for (size_t j = 0; j < num_ops; j++) {
            set_op_t op = (set_op_t) op_distr(prng);
            switch (op) {
                case INSERT:
                    
                    for (size_t k = 0; k < inserts_per_op; ++k) {
                        packed_set.insert(next_val);
                        std_set.insert(next_val);
                        next_val++;
                    }
                    for (size_t k = 0; k < prev_inserts_per_op; ++k) {
                        uint64_t val = prng() % next_val;
                        packed_set.insert(val);
                        std_set.insert(val);
                    }
                    
                    break;
                    
                case REMOVE:
                    if (next_val > 0) {
                        for (size_t k = 0; k < removes_per_op; ++k) {
                            uint64_t val = prng() % next_val;
                            packed_set.remove(val);
                            std_set.erase(val);
                        }
                    }
                    else {
                        packed_set.remove(0);
                        packed_set.remove(1);
                        packed_set.remove(2);
                        std_set.erase(0);
                        std_set.erase(1);
                        std_set.erase(2);
                    }
                    
                    break;
                    
                case FIND:
                    if (next_val) {
                        for (size_t k = 0; k < finds_per_op; k++) {
                            uint64_t val = prng() % next_val;
                            assert(packed_set.find(val) == (bool) std_set.count(val));
                        }
                    }
                    else {
                        assert(packed_set.find(0) == (bool) std_set.count(0));
                        assert(packed_set.find(1) == (bool) std_set.count(1));
                        assert(packed_set.find(2) == (bool) std_set.count(2));
                    }
                    
                    break;
                    
//                case SERIALIZE:
//                {
//                    stringstream strm;
//
//                    dyn_vec.serialize(strm);
//                    strm.seekg(0);
//                    PackedVector<> copy_vec(strm);
//
//                    assert(copy_vec.size() == dyn_vec.size());
//                    for (size_t i = 0; i < copy_vec.size(); i++) {
//                        assert(copy_vec.get(i) == dyn_vec.get(i));
//                    }
//                    break;
//                }
                    
                default:
                    break;
            }
            
            assert(std_set.empty() == packed_set.empty());
            assert(std_set.size() == packed_set.size());
        }
    }
    cerr << "PackedSet tests successful!" << endl;
}

void test_packed_graph() {
    
    auto check_path = [&](MutablePathDeletableHandleGraph& graph, const path_handle_t& p, const vector<handle_t>& steps) {
        assert(graph.get_step_count(p) == steps.size());
        
        step_handle_t step = graph.path_begin(p);
        for (int i = 0; i < steps.size(); i++) {
            
            assert(graph.get_path_handle_of_step(step) == p);
            assert(graph.get_handle_of_step(step) == steps[i]);
            
            if (graph.get_is_circular(p)) {
                assert(graph.has_next_step(step));
                assert(graph.has_previous_step(step));
            }
            else {
                assert(graph.has_next_step(step) == i + 1 < steps.size());
                assert(graph.has_previous_step(step) == i > 0);
            }
            
            step = graph.get_next_step(step);
        }
        
        if (graph.get_is_circular(p) && !graph.is_empty(p)) {
            assert(step == graph.path_begin(p));
        }
        else {
            assert(step == graph.path_end(p));
        }
        
        step = graph.path_back(p);
        
        for (int i = steps.size() - 1; i >= 0; i--) {
            
            assert(graph.get_path_handle_of_step(step) == p);
            assert(graph.get_handle_of_step(step) == steps[i]);
            
            if (graph.get_is_circular(p)) {
                assert(graph.has_next_step(step));
                assert(graph.has_previous_step(step));
            }
            else {
                assert(graph.has_next_step(step) == i + 1 < steps.size());
                assert(graph.has_previous_step(step) == i > 0);
            }
            
            step = graph.get_previous_step(step);
        }
        
        if (graph.get_is_circular(p) && !graph.is_empty(p)) {
            assert(step == graph.path_back(p));
        }
        else {
            assert(step == graph.path_front_end(p));
        }
    };
    
    auto check_flips = [&](MutablePathDeletableHandleGraph& graph, const path_handle_t& p, const vector<handle_t>& steps) {

        auto flipped = steps;
        for (size_t i = 0; i < steps.size(); i++) {
            graph.apply_orientation(graph.flip(graph.forward(flipped[i])));
            flipped[i] = graph.flip(flipped[i]);
            check_path(graph, p, flipped);
            
            graph.apply_orientation(graph.flip(graph.forward(flipped[i])));
            flipped[i] = graph.flip(flipped[i]);
            check_path(graph, p, flipped);
        }
    };
        
    // defragmentation
    {
        PackedGraph graph;
        
        handle_t h1 = graph.create_handle("ATGTAG");
        handle_t h2 = graph.create_handle("ACCCC");
        handle_t h3 = graph.create_handle("C");
        handle_t h4 = graph.create_handle("ATT");
        handle_t h5 = graph.create_handle("GGCA");
        
        graph.create_edge(h1, h2);
        graph.create_edge(h1, h3);
        graph.create_edge(h2, h3);
        graph.create_edge(h3, h5);
        graph.create_edge(h3, h4);
        graph.create_edge(h4, h5);
        
        path_handle_t p0 = graph.create_path_handle("0");
        path_handle_t p1 = graph.create_path_handle("1");
        path_handle_t p2 = graph.create_path_handle("2");
        
        
        graph.append_step(p0, h3);
        graph.append_step(p0, h4);
        graph.append_step(p0, h5);
        
        graph.append_step(p1, h1);
        graph.append_step(p1, h3);
        graph.append_step(p1, h5);
        
        graph.append_step(p2, h1);
        graph.append_step(p2, h2);
        graph.append_step(p2, h3);
        graph.append_step(p2, h4);
        graph.append_step(p2, h5);
        
        graph.destroy_path(p0);
        graph.destroy_path(p2);
        graph.destroy_handle(h2);
        graph.destroy_handle(h4);
        
        assert(graph.get_sequence(h1) == "ATGTAG");
        assert(graph.get_sequence(h3) == "C");
        assert(graph.get_sequence(h5) == "GGCA");
        
        bool found = false;
        graph.follow_edges(h1, false, [&](const handle_t& next) {
            if (next == h3) {
                found = true;
            }
            else {
                assert(false);
            }
            return true;
        });
        assert(found);
        
        found = false;
        graph.follow_edges(h3, false, [&](const handle_t& next) {
            if (next == h5) {
                found = true;
            }
            else {
                assert(false);
            }
            return true;
        });
        assert(found);
        
        check_flips(graph, p1, {h1, h3, h5});
    }
    
    // tightening vector allocations
    {
        PackedGraph graph;
        handle_t h1 = graph.create_handle("ATGTAG");
        handle_t h2 = graph.create_handle("ACCCC");
        handle_t h3 = graph.create_handle("C");
        handle_t h4 = graph.create_handle("ATT");
        handle_t h5 = graph.create_handle("GGCA");
        
        graph.create_edge(h1, h2);
        graph.create_edge(h1, h3);
        graph.create_edge(h2, h3);
        graph.create_edge(h3, h5);
        graph.create_edge(h3, h4);
        graph.create_edge(h4, h5);
        
        path_handle_t p0 = graph.create_path_handle("0");
        path_handle_t p1 = graph.create_path_handle("1");
        path_handle_t p2 = graph.create_path_handle("2");
        
        
        graph.append_step(p0, h3);
        graph.append_step(p0, h4);
        graph.append_step(p0, h5);
        
        graph.append_step(p1, h1);
        graph.append_step(p1, h3);
        graph.append_step(p1, h5);
        
        graph.append_step(p2, h1);
        graph.append_step(p2, h2);
        graph.append_step(p2, h3);
        graph.append_step(p2, h4);
        graph.append_step(p2, h5);
        
        // delete some things, but not enough to trigger defragmentation
        graph.destroy_path(p2);
        graph.destroy_handle(h2);
        // reallocate and compress down to the smaller size
        graph.optimize(false);
        
        assert(graph.get_sequence(h1) == "ATGTAG");
        assert(graph.get_sequence(h3) == "C");
        assert(graph.get_sequence(h4) == "ATT");
        assert(graph.get_sequence(h5) == "GGCA");
        
        int count = 0;
        bool found1 = false, found2 = false;
        graph.follow_edges(h1, false, [&](const handle_t& h) {
            if (h == h3) {
                found1 = true;
            }
            count++;
        });
        assert(found1);
        assert(count == 1);
        
        count = 0;
        found1 = false, found2 = false;
        graph.follow_edges(h1, true, [&](const handle_t& h) {
            count++;
        });
        assert(count == 0);
        
        count = 0;
        found1 = false, found2 = false;
        graph.follow_edges(h3, false, [&](const handle_t& h) {
            if (h == h4) {
                found1 = true;
            }
            if (h == h5) {
                found2 = true;
            }
            count++;
        });
        assert(found1);
        assert(found2);
        assert(count == 2);
        
        count = 0;
        found1 = false, found2 = false;
        graph.follow_edges(h3, true, [&](const handle_t& h) {
            if (h == h1) {
                found1 = true;
            }
            count++;
        });
        assert(found1);
        assert(count == 1);
        
        count = 0;
        found1 = false, found2 = false;
        graph.follow_edges(h4, false, [&](const handle_t& h) {
            if (h == h5) {
                found1 = true;
            }
            count++;
        });
        assert(found1);
        assert(count == 1);
        
        count = 0;
        found1 = false, found2 = false;
        graph.follow_edges(h4, true, [&](const handle_t& h) {
            if (h == h3) {
                found1 = true;
            }
            count++;
        });
        assert(found1);
        assert(count == 1);
        
        count = 0;
        found1 = false, found2 = false;
        graph.follow_edges(h5, false, [&](const handle_t& h) {
            count++;
        });
        assert(count == 0);
        
        count = 0;
        found1 = false, found2 = false;
        graph.follow_edges(h5, true, [&](const handle_t& h) {
            if (h == h3) {
                found1 = true;
            }
            else if (h == h4) {
                found2 = true;
            }
            count++;
        });
        assert(found1);
        assert(found2);
        assert(count == 2);
        
        check_flips(graph, p0, {h3, h4, h5});
        check_flips(graph, p1, {h1, h3, h5});
    }
    
    // optimizing with id reassignment
    {
        PackedGraph graph;
        handle_t h1 = graph.create_handle("ATGTAG");
        handle_t h2 = graph.create_handle("ACCCC");
        handle_t h3 = graph.create_handle("C");
        handle_t h4 = graph.create_handle("ATT");
        handle_t h5 = graph.create_handle("GGCA");
        
        graph.create_edge(h1, h2);
        graph.create_edge(h1, h3);
        graph.create_edge(h2, h3);
        graph.create_edge(h3, h5);
        graph.create_edge(h3, h4);
        graph.create_edge(h4, h5);
        
        path_handle_t p0 = graph.create_path_handle("0");
        path_handle_t p1 = graph.create_path_handle("1");
        path_handle_t p2 = graph.create_path_handle("2");
        
        
        graph.append_step(p0, h3);
        graph.append_step(p0, h4);
        graph.append_step(p0, h5);
        
        graph.append_step(p1, h1);
        graph.append_step(p1, h3);
        graph.append_step(p1, h5);
        
        graph.append_step(p2, h1);
        graph.append_step(p2, h2);
        graph.append_step(p2, h3);
        graph.append_step(p2, h4);
        graph.append_step(p2, h5);
        
        // delete some things, but not enough to trigger defragmentation
        graph.destroy_path(p2);
        graph.destroy_handle(h2);
        // reallocate and compress down to the smaller size, reassigning IDs
        graph.optimize(true);
        set<nid_t> seen_ids;
        
        int count = 0;
        bool found1 = false, found2 = false, found3 = false, found4 = false;
        graph.for_each_handle([&](const handle_t& handle) {
            if (graph.get_sequence(handle) == "ATGTAG") {
                h1 = handle;
                found1 = true;
            }
            else if (graph.get_sequence(handle) == "C") {
                h3 = handle;
                found2 = true;
            }
            else if (graph.get_sequence(handle) == "ATT") {
                h4 = handle;
                found3 = true;
            }
            else if (graph.get_sequence(handle) == "GGCA") {
                h5 = handle;
                found4 = true;
            }
            else {
                assert(false);
            }
            count++;
            
            seen_ids.insert(graph.get_id(handle));
            
            assert(graph.get_id(handle) >= 1);
            assert(graph.get_id(handle) <= 4);
        });
        
        assert(found1);
        assert(found2);
        assert(found3);
        assert(found4);
        assert(count == 4);
        assert(seen_ids.size() == 4);
        
        count = 0;
        found1 = found2 = found3 = found4 = false;
        
        graph.follow_edges(h1, false, [&](const handle_t& h) {
            if (h == h3) {
                found1 = true;
            }
            count++;
        });
        assert(found1);
        assert(count == 1);
        
        count = 0;
        found1 = false, found2 = false;
        graph.follow_edges(h1, true, [&](const handle_t& h) {
            count++;
        });
        assert(count == 0);
        
        count = 0;
        found1 = false, found2 = false;
        graph.follow_edges(h3, false, [&](const handle_t& h) {
            if (h == h4) {
                found1 = true;
            }
            if (h == h5) {
                found2 = true;
            }
            count++;
        });
        assert(found1);
        assert(found2);
        assert(count == 2);
        
        count = 0;
        found1 = false, found2 = false;
        graph.follow_edges(h3, true, [&](const handle_t& h) {
            if (h == h1) {
                found1 = true;
            }
            count++;
        });
        assert(found1);
        assert(count == 1);
        
        count = 0;
        found1 = false, found2 = false;
        graph.follow_edges(h4, false, [&](const handle_t& h) {
            if (h == h5) {
                found1 = true;
            }
            count++;
        });
        assert(found1);
        assert(count == 1);
        
        count = 0;
        found1 = false, found2 = false;
        graph.follow_edges(h4, true, [&](const handle_t& h) {
            if (h == h3) {
                found1 = true;
            }
            count++;
        });
        assert(found1);
        assert(count == 1);
        
        count = 0;
        found1 = false, found2 = false;
        graph.follow_edges(h5, false, [&](const handle_t& h) {
            count++;
        });
        assert(count == 0);
        
        count = 0;
        found1 = false, found2 = false;
        graph.follow_edges(h5, true, [&](const handle_t& h) {
            if (h == h3) {
                found1 = true;
            }
            else if (h == h4) {
                found2 = true;
            }
            count++;
        });
        assert(found1);
        assert(found2);
        assert(count == 2);
        
        check_flips(graph, p0, {h3, h4, h5});
        check_flips(graph, p1, {h1, h3, h5});
    }
    
    cerr << "PackedGraph tests successful!" << endl;
}

void test_path_position_overlays() {
    
    PackedGraph pg;
    HashGraph hg;
    ODGI og;
    
    vector<MutablePathDeletableHandleGraph*> implementations;
    implementations.push_back(&pg);
    implementations.push_back(&hg);
    implementations.push_back(&og);
    
    for (MutablePathDeletableHandleGraph* implementation : implementations) {
        
        MutablePathDeletableHandleGraph& graph = *implementation;
        
        handle_t h1 = graph.create_handle("AAA");
        handle_t h2 = graph.create_handle("A");
        handle_t h3 = graph.create_handle("T");
        handle_t h4 = graph.create_handle("AAAAA");
        
        graph.create_edge(h1, h2);
        graph.create_edge(h1, h3);
        graph.create_edge(h2, h4);
        graph.create_edge(h3, h4);
        
        path_handle_t p1 = graph.create_path_handle("p1");
        step_handle_t s1 = graph.append_step(p1, h1);
        step_handle_t s2 = graph.append_step(p1, h2);
        step_handle_t s3 = graph.append_step(p1, h4);
        
        // static position overlays
        {
            vector<PathPositionHandleGraph*> overlays;
            
            PositionOverlay basic_overlay(&graph);
            PackedPositionOverlay packed_overlay(&graph);
            
            overlays.push_back(&basic_overlay);
            overlays.push_back(&packed_overlay);
            
            for (PathPositionHandleGraph* implementation : overlays) {
                PathPositionHandleGraph& overlay = *implementation;
                
                assert(overlay.get_path_length(p1) == 9);
                
                assert(overlay.get_position_of_step(s1) == 0);
                assert(overlay.get_position_of_step(s2) == 3);
                assert(overlay.get_position_of_step(s3) == 4);
                
                assert(overlay.get_step_at_position(p1, 0) == s1);
                assert(overlay.get_step_at_position(p1, 1) == s1);
                assert(overlay.get_step_at_position(p1, 2) == s1);
                assert(overlay.get_step_at_position(p1, 3) == s2);
                assert(overlay.get_step_at_position(p1, 4) == s3);
                assert(overlay.get_step_at_position(p1, 5) == s3);
                assert(overlay.get_step_at_position(p1, 6) == s3);
                assert(overlay.get_step_at_position(p1, 7) == s3);
                assert(overlay.get_step_at_position(p1, 8) == s3);
                assert(overlay.get_step_at_position(p1, 9) == overlay.path_end(p1));
            }
        }
        
        
        // mutable position overlay
        {
            MutablePositionOverlay overlay(&graph);
            
            handle_t h5 = overlay.create_handle("AAAA");
            
            overlay.create_edge(h4, h5);
            overlay.create_edge(h5, h5);
            
            step_handle_t s4 = overlay.append_step(p1, h5);
            
            assert(overlay.get_path_length(p1) == 13);
            
            assert(overlay.get_position_of_step(s4) == 9);
            
            assert(overlay.get_step_at_position(p1, 9) == s4);
            assert(overlay.get_step_at_position(p1, 10) == s4);
            assert(overlay.get_step_at_position(p1, 11) == s4);
            assert(overlay.get_step_at_position(p1, 12) == s4);
            assert(overlay.get_step_at_position(p1, 13) == overlay.path_end(p1));
            
            step_handle_t s5 = overlay.append_step(p1, h5);
            
            assert(overlay.get_path_length(p1) == 17);
            
            assert(overlay.get_position_of_step(s5) == 13);
            
            assert(overlay.get_step_at_position(p1, 13) == s5);
            assert(overlay.get_step_at_position(p1, 14) == s5);
            assert(overlay.get_step_at_position(p1, 15) == s5);
            assert(overlay.get_step_at_position(p1, 16) == s5);
            assert(overlay.get_step_at_position(p1, 17) == overlay.path_end(p1));
            
            path_handle_t p2 = overlay.create_path_handle("p2");
            
            assert(overlay.get_path_length(p2) == 0);
            
            step_handle_t s6 = overlay.prepend_step(p2, h3);
            
            assert(overlay.get_path_length(p2) == 1);
            
            assert(overlay.get_position_of_step(s6) == 0);
            
            assert(overlay.get_step_at_position(p2, 0) == s6);
            assert(overlay.get_step_at_position(p2, 1) == overlay.path_end(p2));
            
            step_handle_t s7 = overlay.prepend_step(p2, h1);
            
            assert(overlay.get_path_length(p2) == 4);
            
            assert(overlay.get_position_of_step(s7) == 0);
            assert(overlay.get_position_of_step(s6) == 3);
            
            assert(overlay.get_step_at_position(p2, 0) == s7);
            assert(overlay.get_step_at_position(p2, 1) == s7);
            assert(overlay.get_step_at_position(p2, 2) == s7);
            assert(overlay.get_step_at_position(p2, 3) == s6);
            assert(overlay.get_step_at_position(p2, 4) == overlay.path_end(p2));
            
            handle_t h2_flip = overlay.apply_orientation(overlay.flip(h2));
            assert(overlay.get_handle_of_step(overlay.get_step_at_position(p1, 3)) == overlay.flip(h2_flip));
            
            vector<size_t> offs_1{1};
            auto parts_1 = overlay.divide_handle(overlay.flip(h1), offs_1);
            assert(overlay.get_handle_of_step(overlay.get_step_at_position(p1, 0)) == overlay.flip(parts_1[1]));
            assert(overlay.get_handle_of_step(overlay.get_step_at_position(p1, 1)) == overlay.flip(parts_1[1]));
            assert(overlay.get_handle_of_step(overlay.get_step_at_position(p1, 2)) == overlay.flip(parts_1[0]));
            assert(overlay.get_handle_of_step(overlay.get_step_at_position(p1, 3)) == overlay.flip(h2_flip));
            assert(overlay.get_handle_of_step(overlay.get_step_at_position(p2, 0)) == overlay.flip(parts_1[1]));
            assert(overlay.get_handle_of_step(overlay.get_step_at_position(p2, 1)) == overlay.flip(parts_1[1]));
            assert(overlay.get_handle_of_step(overlay.get_step_at_position(p2, 2)) == overlay.flip(parts_1[0]));
            assert(overlay.get_handle_of_step(overlay.get_step_at_position(p2, 3)) == h3);
            
            
            vector<size_t> offs_2{1, 3};
            auto parts_2 = overlay.divide_handle(h5, offs_2);
            assert(overlay.get_handle_of_step(overlay.get_step_at_position(p1, 9)) == parts_2[0]);
            assert(overlay.get_handle_of_step(overlay.get_step_at_position(p1, 10)) == parts_2[1]);
            assert(overlay.get_handle_of_step(overlay.get_step_at_position(p1, 11)) == parts_2[1]);
            assert(overlay.get_handle_of_step(overlay.get_step_at_position(p1, 12)) == parts_2[2]);
            assert(overlay.get_handle_of_step(overlay.get_step_at_position(p1, 13)) == parts_2[0]);
            assert(overlay.get_handle_of_step(overlay.get_step_at_position(p1, 14)) == parts_2[1]);
            assert(overlay.get_handle_of_step(overlay.get_step_at_position(p1, 15)) == parts_2[1]);
            assert(overlay.get_handle_of_step(overlay.get_step_at_position(p1, 16)) == parts_2[2]);
            assert(overlay.get_step_at_position(p1, 17) == overlay.path_end(p1));
        }
    }
    cerr << "PathPositionOverlay tests successful!" << endl;
}

void test_vectorizable_overlays() {
    
    bdsg::PackedGraph pg;
    bdsg::HashGraph hg;
    bdsg::ODGI og;
    
    vector<MutablePathDeletableHandleGraph*> implementations;
    implementations.push_back(&pg);
    implementations.push_back(&hg);
    implementations.push_back(&og);
    
    for (MutablePathDeletableHandleGraph* implementation : implementations) {
        
        MutablePathDeletableHandleGraph& graph = *implementation;
        
        handle_t h1 = graph.create_handle("AAA");
        handle_t h2 = graph.create_handle("A");
        handle_t h3 = graph.create_handle("T");
        handle_t h4 = graph.create_handle("AAAAA");
        
        graph.create_edge(h1, h2);
        graph.create_edge(h1, h3);
        graph.create_edge(h2, h4);
        graph.create_edge(h3, h4);
        
        path_handle_t p1 = graph.create_path_handle("p1");
        step_handle_t s1 = graph.append_step(p1, h1);
        step_handle_t s2 = graph.append_step(p1, h2);
        step_handle_t s3 = graph.append_step(p1, h4);

        
        bdsg::VectorizableOverlay overlay(&graph);

        set<size_t> edge_ranks;
        size_t edge_count = 0;
        graph.for_each_edge([&](edge_t edge) {
            edge_ranks.insert(overlay.edge_index(edge));
            ++edge_count;
          });

        // every edge gets a unique rank
        assert(edge_ranks.size() == edge_count);

        size_t node_count = 0;
        map<size_t, nid_t> pos_to_node;
        graph.for_each_handle([&](handle_t handle) {
                pos_to_node[overlay.node_vector_offset(graph.get_id(handle))] = graph.get_id(handle);
                ++node_count;
          });

        // every node gets a unique rank
        assert(pos_to_node.size() == node_count);

        auto pni = pos_to_node.begin();
        auto next = pni;
        for (++next; next != pos_to_node.end(); ++pni, ++next) {
            assert(next->first - pni->first == graph.get_length(graph.get_handle(pni->second)));
        }

        // check that node_at_vector_offset works
        graph.for_each_handle([&](handle_t handle) {
                size_t pos = overlay.node_vector_offset(graph.get_id(handle));
                for (size_t i = 0; i < graph.get_length(handle); ++i) {
                    assert(overlay.node_at_vector_offset(pos + i + 1) == graph.get_id(handle));
                }
            });
    }
    cerr << "VectorizableOverlay tests successful!" << endl;
}

void test_packed_subgraph_overlay() {
    
    bdsg::PackedGraph pg;
    bdsg::HashGraph hg;
    bdsg::ODGI og;
    
    vector<MutablePathDeletableHandleGraph*> implementations;
    implementations.push_back(&pg);
    implementations.push_back(&hg);
    implementations.push_back(&og);
    
    for (MutablePathDeletableHandleGraph* implementation : implementations) {

        MutablePathDeletableHandleGraph& graph = *implementation;
        
        handle_t h1 = graph.create_handle("AAA");
        handle_t h2 = graph.create_handle("A");
        handle_t h3 = graph.create_handle("T");
        handle_t h4 = graph.create_handle("AAAAA");
        
        graph.create_edge(h1, h2);
        graph.create_edge(h1, h3);
        graph.create_edge(h2, h4);
        graph.create_edge(h3, h4);
        
        PackedSubgraphOverlay subgraph(&graph);
        assert(subgraph.get_node_count() == 0);
        subgraph.for_each_handle([&](const handle_t& h) {
            assert(false);
        });
        assert(!subgraph.has_node(graph.get_id(h1)));
        assert(!subgraph.has_node(graph.get_id(h2)));
        assert(!subgraph.has_node(graph.get_id(h3)));
        assert(!subgraph.has_node(graph.get_id(h4)));
        
        subgraph.add_node(h1);
        
        assert(subgraph.get_node_count() == 1);
        bool found1 = false;
        subgraph.for_each_handle([&](const handle_t& h) {
            if (subgraph.get_id(h) == graph.get_id(h1)) {
                found1 = true;
                assert(graph.get_sequence(h) == graph.get_sequence(h1));
            }
            else {
                assert(false);
            }
        });
        assert(found1);
        found1 = false;
        
        assert(subgraph.has_node(graph.get_id(h1)));
        assert(!subgraph.has_node(graph.get_id(h2)));
        assert(!subgraph.has_node(graph.get_id(h3)));
        assert(!subgraph.has_node(graph.get_id(h4)));
        
        subgraph.follow_edges(h1, true, [&](const handle_t& h) {
            assert(false);
        });
        subgraph.follow_edges(h1, false, [&](const handle_t& h) {
            assert(false);
        });
        
        assert(subgraph.get_degree(h1, true) == 0);
        assert(subgraph.get_degree(h1, false) == 0);
        
        subgraph.add_node(h4);

        assert(subgraph.get_node_count() == 2);
        bool found2 = false;
        subgraph.for_each_handle([&](const handle_t& h) {
            if (subgraph.get_id(h) == graph.get_id(h1)) {
                found1 = true;
                assert(graph.get_sequence(h) == graph.get_sequence(h1));
            }
            else if (subgraph.get_id(h) == graph.get_id(h4)) {
                found2 = true;
                assert(graph.get_sequence(h) == graph.get_sequence(h4));
            }
            else {
                assert(false);
            }
        });
        assert(found1);
        assert(found2);
        found1 = false;
        found2 = false;
        
        assert(subgraph.has_node(graph.get_id(h1)));
        assert(!subgraph.has_node(graph.get_id(h2)));
        assert(!subgraph.has_node(graph.get_id(h3)));
        assert(subgraph.has_node(graph.get_id(h4)));
        
        subgraph.follow_edges(h1, true, [&](const handle_t& h) {
            assert(false);
        });
        subgraph.follow_edges(h1, false, [&](const handle_t& h) {
            assert(false);
        });
        subgraph.follow_edges(h4, true, [&](const handle_t& h) {
            assert(false);
        });
        subgraph.follow_edges(h4, false, [&](const handle_t& h) {
            assert(false);
        });
        
        
        assert(subgraph.get_degree(h1, true) == 0);
        assert(subgraph.get_degree(h1, false) == 0);
        assert(subgraph.get_degree(h4, true) == 0);
        assert(subgraph.get_degree(h4, false) == 0);
        
        subgraph.add_node(graph.flip(h2));
        
        assert(subgraph.get_node_count() == 3);
        bool found3 = false;
        subgraph.for_each_handle([&](const handle_t& h) {
            if (subgraph.get_id(h) == graph.get_id(h1)) {
                found1 = true;
                assert(graph.get_sequence(h) == graph.get_sequence(h1));
            }
            else if (subgraph.get_id(h) == graph.get_id(h2)) {
                found2 = true;
                assert(graph.get_sequence(h) == graph.get_sequence(h2));
            }
            else if (subgraph.get_id(h) == graph.get_id(h4)) {
                found3 = true;
                assert(graph.get_sequence(h) == graph.get_sequence(h4));
            }
            else {
                assert(false);
            }
        });
        assert(found1);
        assert(found2);
        assert(found3);
        found1 = false;
        found2 = false;
        found3 = false;
        
        assert(subgraph.has_node(graph.get_id(h1)));
        assert(subgraph.has_node(graph.get_id(h2)));
        assert(!subgraph.has_node(graph.get_id(h3)));
        assert(subgraph.has_node(graph.get_id(h4)));
        
        bool found4 = false;
        subgraph.follow_edges(h1, true, [&](const handle_t& h) {
            assert(false);
        });
        subgraph.follow_edges(h1, false, [&](const handle_t& h) {
            if (subgraph.get_id(h) == graph.get_id(h2) && !graph.get_is_reverse(h)) {
                found1 = true;
            }
            else {
                assert(false);
            }
        });
        subgraph.follow_edges(h2, true, [&](const handle_t& h) {
            if (subgraph.get_id(h) == graph.get_id(h1) && !graph.get_is_reverse(h)) {
                found2 = true;
            }
            else {
                assert(false);
            }
        });
        subgraph.follow_edges(h2, false, [&](const handle_t& h) {
            if (subgraph.get_id(h) == graph.get_id(h4) && !graph.get_is_reverse(h)) {
                found3 = true;
            }
            else {
                assert(false);
            }
        });
        subgraph.follow_edges(h4, true, [&](const handle_t& h) {
            if (subgraph.get_id(h) == graph.get_id(h2) && !graph.get_is_reverse(h)) {
                found4 = true;
            }
            else {
                assert(false);
            }
        });
        subgraph.follow_edges(h4, false, [&](const handle_t& h) {
            assert(false);
        });
        
        assert(subgraph.get_degree(h1, true) == 0);
        assert(subgraph.get_degree(h1, false) == 1);
        assert(subgraph.get_degree(h2, true) == 1);
        assert(subgraph.get_degree(h2, false) == 1);
        assert(subgraph.get_degree(h4, true) == 1);
        assert(subgraph.get_degree(h4, false) == 0);
        
        assert(found1);
        assert(found2);
        assert(found3);
        assert(found4);
        found1 = false;
        found2 = false;
        found3 = false;
        found4 = false;
        
        subgraph.remove_node(h1);
        
        assert(subgraph.get_node_count() == 2);
        subgraph.for_each_handle([&](const handle_t& h) {
            if (subgraph.get_id(h) == graph.get_id(h2)) {
                found1 = true;
                assert(graph.get_sequence(h) == graph.get_sequence(h2));
            }
            else if (subgraph.get_id(h) == graph.get_id(h4)) {
                found2 = true;
                assert(graph.get_sequence(h) == graph.get_sequence(h4));
            }
            else {
                assert(false);
            }
        });
        assert(found1);
        assert(found2);
        found1 = false;
        found2 = false;
        
        assert(!subgraph.has_node(graph.get_id(h1)));
        assert(subgraph.has_node(graph.get_id(h2)));
        assert(!subgraph.has_node(graph.get_id(h3)));
        assert(subgraph.has_node(graph.get_id(h4)));
        
        subgraph.follow_edges(h2, true, [&](const handle_t& h) {
            assert(false);
        });
        subgraph.follow_edges(h2, false, [&](const handle_t& h) {
            if (subgraph.get_id(h) == graph.get_id(h4) && !graph.get_is_reverse(h)) {
                found1 = true;
            }
            else {
                assert(false);
            }
        });
        subgraph.follow_edges(h4, true, [&](const handle_t& h) {
            if (subgraph.get_id(h) == graph.get_id(h2) && !graph.get_is_reverse(h)) {
                found2 = true;
            }
            else {
                assert(false);
            }
        });
        subgraph.follow_edges(h4, false, [&](const handle_t& h) {
            assert(false);
        });
        
        assert(subgraph.get_degree(h2, true) == 0);
        assert(subgraph.get_degree(h2, false) == 1);
        assert(subgraph.get_degree(h4, true) == 1);
        assert(subgraph.get_degree(h4, false) == 0);
        
        assert(found1);
        assert(found2);
        found1 = false;
        found2 = false;
    }
    
    cerr << "PackedSubgraphOverlay tests successful!" << endl;
}


int main(void) {
    test_mmap_backend();
    test_mapped_structs();
    test_packed_vector<PackedVector<>>();
    test_packed_vector<PackedVector<CompatIntVector<>>>();
    test_paged_vector<PagedVector<1>>();
    test_paged_vector<PagedVector<2>>();
    test_paged_vector<PagedVector<3>>();
    test_paged_vector<PagedVector<4>>();
    test_paged_vector<PagedVector<5>>();
    test_paged_vector<PagedVector<5, PackedVector<CompatIntVector<>>, CompatVector<PackedVector<CompatIntVector<>>>>>();
    test_packed_deque();
    test_packed_set();
    test_deletable_handle_graphs();
    test_mutable_path_handle_graphs();
    test_serializable_handle_graphs();
    test_packed_graph();
    test_path_position_overlays();
    test_vectorizable_overlays();
    test_packed_subgraph_overlay();
}
