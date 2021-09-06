#include "client.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
// #include "neighborhoods_generated.h"
#include <thread>
#include <unistd.h>
#include <iostream>
#include <algorithm>
#include "std_msgs/Int16MultiArray.h"
#include "std_msgs/MultiArrayDimension.h"
#include "eigen3/Eigen/Dense"

Client::Client(clientutils::params_t params, ros::NodeHandle n) : params(params), n(n)
{
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
    {
        perror("socket creation failed");
        exit(EXIT_FAILURE);
    }

    client_busy = false;
    set_nodes();

    routing_table_pub = n.advertise<std_msgs::Int16MultiArray>("/routing_table", 10);
}

recv_data_t *Client::send_recv_data()
{
    client_busy = true;
    socklen_t len;
    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));

    // Filling server information
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(params.port);
    servaddr.sin_addr.s_addr = INADDR_ANY;

    sendto(sockfd, data, data_size,
           MSG_CONFIRM, (const struct sockaddr *)&servaddr,
           sizeof(servaddr));
    ROS_DEBUG_STREAM("Sent " << data_size << " bytes of data to the NS3 server.");
    ROS_DEBUG_STREAM("Waiting for the server to respond...");

    // char buffer[MAXLINE];

    n_bytes = recvfrom(sockfd, recv_buffer, MAXLINE,
                       MSG_WAITALL, (struct sockaddr *)&servaddr,
                       &len);
    recv_data_t routing_data;
    routing_data.n_bytes = n_bytes;

    routing_data.recv_buffer = recv_buffer;
    client_busy = false;
    return &routing_data;
    // close(sockfd);
}


void Client::run()
{
    ros::Timer timer = n.createTimer(ros::Duration(1 / params.frequency), &Client::iteration, this);
    ros::spin();
}

void Client::set_nodes()
{
    // utils::nodes_t nodes_;
    // nodes.clear();
    for (int i = 0; i < params.n_robots; i++)
    {
        utils::Node *node = new utils::Node(i + 1, n, i<params.n_backbone);
        nodes.push_back(node);
    }
    ROS_DEBUG_STREAM("Created " << params.n_robots << " with "<<params.n_backbone << " backbones.");
}

void Client::iteration(const ros::TimerEvent &e)
{
    //get data from subscribers
    if (!client_busy)
    {
        ROS_DEBUG_STREAM("Iteration performing.");

        flatbuffers::FlatBufferBuilder builder;
        agents_t agents;

        for (int i = 0; i < params.n_robots; i++)
        {
            simulator_utils::Waypoint state = nodes[i]->get_state();
            // ROS_DEBUG_STREAM(state.position.x << state.position.y << state.position.z);
            auto pos = Vec3(state.position.x, state.position.y, state.position.z);
            auto id = i;
            auto agent = CreateAgent(builder, &pos, id);
            agents.push_back(agent);
        }

        //create the swarm object
        auto agents_ = builder.CreateVector(agents);
        auto swarm = CreateSwarm(builder, params.n_backbone, agents_);
        builder.Finish(swarm);
        data = builder.GetBufferPointer();
        data_size = builder.GetSize();

        // send data to the server in a non-blocking call (if the previous call to the
        // server is completed), wait for the response. Write the response to the variable.
        auto con = [this]()
        {
            this->send_recv_data();
            routing_tables = this->set_network();
        };
        new std::thread(con);
    }
    else
    {
        ROS_DEBUG_STREAM("Iteration skipping. Client busy");
    }

    publish_routing_table();
    // publish current routing nodes
    // for(clientutils::Node* n: nodes) {
    //     if (n->is_backbone()) {
    //         n->publish_routing_nodes();
    //         ROS_DEBUG_STREAM("Published routing tables.");
    //     }
    // }
}

routing_table_t Client::set_network() {
    routing_table_t routing_table;
    char recvd_data[n_bytes];
    std::memcpy(recvd_data, recv_buffer, sizeof(recvd_data));
    ROS_DEBUG_STREAM("Received " << sizeof(recvd_data) << " bytes.");
    routing_table.clear();

    // receive the router tables as is from server
    auto swarmnetwork = GetSwarmNetwork(recvd_data);
    // auto network_nodes = swarmnetwork->nodes();
    auto networknodes = swarmnetwork->nodes();
    for(int i=0; i<networknodes->size(); i++) {
        std::vector<int> routing_nodes;
        auto routingtable = networknodes->Get(i)->routingtable();
        for (int j=0;j<routingtable->size(); j++) {
            auto entry = routingtable->Get(j);
            int hops = entry->distance();
            if (hops == params.hops_k) {
                routing_nodes.push_back(entry->destination());
            }
        }
        // nodes[i]->set_routing_nodes(routing_nodes);
        // set the routing nodes into a multi-dimensional vector
        routing_table.push_back(routing_nodes);
    }
    return routing_table;
}

// create adjacency matrix -> use it to initialize the multi-array
void Client::publish_routing_table() {
    // first create the adjacency matrix
    Eigen::MatrixXi adjacency = Eigen::MatrixXi::Zero(params.n_backbone,params.n_backbone);

    for(int i=0; i<routing_tables.size(); i++) {
        std::vector<int> table_i = routing_tables[i];
        // adjacency(i,i) = 1;
        for(int j=0; j<params.n_backbone; j++) {
            if(clientutils::has_value(table_i, j)) {
                adjacency(i,j) = 1;
            }            
        }
    }
    // std::cout << "adjacency mat: "<<adjacency <<std::endl;
// use adjacency matrix to populate the publising message

    std_msgs::Int16MultiArray msg;

    msg.layout.dim.push_back(std_msgs::MultiArrayDimension());
    msg.layout.dim.push_back(std_msgs::MultiArrayDimension());

    msg.layout.dim[0].size = params.n_backbone;
    msg.layout.dim[0].stride = params.n_backbone*params.n_backbone;
    msg.layout.dim[0].label = "rows";
    
    msg.layout.dim[1].size = params.n_backbone;
    msg.layout.dim[1].stride = params.n_backbone;
    msg.layout.dim[1].label = "columns";
    
    for(int i=0;i<adjacency.size(); i++) {
        msg.data.push_back(adjacency.data()[i]);
        std::cout<<adjacency.data()[i];
    }

    routing_table_pub.publish(msg);
}

// calculate the average neighborhood hops against distance
// void Client::calc_plot_info()
// {
//     auto network_nodes = this->network;
//     // get avg hops between the robots in the network
//     std::vector<int> hops;
//     std::vector<int> entries;

//     for (int i = 0; i < network_nodes->size(); i++)
//     {
//         int n_id = network_nodes->Get(i)->node();
//         auto routingtable = network_nodes->Get(i)->routingtable();
//         // ROS_INFO_STREAM("node: " << n_id << " table entries: " << routingtable->size());
//         entries.push_back(routingtable->size());

//         for (int j = 0; j < routingtable->size(); j++)
//         {
//             auto entry = routingtable->Get(j);
//             if (entry->destination() != i)
//             {
//                 hops.push_back(entry->distance());
//             }
//         }
//         if (routingtable->size() < 7)
//         {
//             int balance = 7 - routingtable->size();
//             for (int j = 0; j < balance; j++)
//             {
//                 hops.push_back(0);
//             }
//         }
//     }
//     //get avg distance between nodes
//     double avg_dis = clientutils::get_avg_dist(nodes);
//     clientutils::write_to_file(hops, avg_dis);
//     ROS_INFO_STREAM("avg_dis: "<<avg_dis<<" wrote hops info to file.");

//     // for(int n_entrees: entries) {
//     //     std::cout << " "<< n_entrees;
//     // }
//     // std::cout << std::endl;
// }