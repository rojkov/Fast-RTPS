// Copyright 2016 Proyectos y Sistemas de Mantenimiento SL (eProsima).
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file PubSubWriter.hpp
 *
 */

#ifndef _TEST_BLACKBOX_PUBSUBWRITER_HPP_
#define _TEST_BLACKBOX_PUBSUBWRITER_HPP_

#include <fastrtps/fastrtps_fwd.h>
#include <fastrtps/Domain.h>
#include <fastrtps/participant/Participant.h>
#include <fastrtps/attributes/ParticipantAttributes.h>
#include <fastrtps/publisher/Publisher.h>
#include <fastrtps/publisher/PublisherListener.h>
#include <fastrtps/attributes/PublisherAttributes.h>
#include <fastrtps/rtps/common/Locator.h>
#include <string>
#include <list>
#include <condition_variable>
#include <boost/asio.hpp>
#include <boost/interprocess/detail/os_thread_functions.hpp>
#include <gtest/gtest.h>

template<class TypeSupport>
class PubSubWriter 
{
    class Listener : public eprosima::fastrtps::PublisherListener
    {
        public:

            Listener(PubSubWriter &writer) : writer_(writer){};

            ~Listener(){};

            void onPublicationMatched(eprosima::fastrtps::Publisher* /*pub*/, MatchingInfo &info)
            {
                if (info.status == MATCHED_MATCHING)
                    writer_.matched();
                else
                    writer_.unmatched();
            }

        private:

            Listener& operator=(const Listener&) NON_COPYABLE_CXX11;

            PubSubWriter &writer_;

    } listener_;

    public:

    typedef TypeSupport type_support;
    typedef typename type_support::type type;

    PubSubWriter(const std::string &topic_name) : listener_(*this), participant_(nullptr),
    publisher_(nullptr), topic_name_(topic_name), initialized_(false), matched_(0)
    {
        publisher_attr_.topic.topicDataType = type_.getName();
        // Generate topic name
        std::ostringstream t;
        t << topic_name_ << "_" << boost::asio::ip::host_name() << "_" << boost::interprocess::ipcdetail::get_current_process_id();
        publisher_attr_.topic.topicName = t.str();

#if defined(PREALLOCATED_WITH_REALLOC_MEMORY_MODE_TEST)
        publisher_attr_.historyMemoryPolicy = PREALLOCATED_WITH_REALLOC_MEMORY_MODE;
#elif defined(DYNAMIC_RESERVE_MEMORY_MODE_TEST)
        publisher_attr_.historyMemoryPolicy = DYNAMIC_RESERVE_MEMORY_MODE;
#else
        publisher_attr_.historyMemoryPolicy = PREALLOCATED_MEMORY_MODE;
#endif

        // By default, heartbeat period and nack response delay are 100 milliseconds.
        publisher_attr_.times.heartbeatPeriod.seconds = 0;
        publisher_attr_.times.heartbeatPeriod.fraction = 4294967 * 100;
        publisher_attr_.times.nackResponseDelay.seconds = 0;
        publisher_attr_.times.nackResponseDelay.fraction = 4294967 * 100;
    }

    ~PubSubWriter()
    {
        if(participant_ != nullptr)
            eprosima::fastrtps::Domain::removeParticipant(participant_);
    }

    void init()
    {
        //Create participant
        participant_attr_.rtps.builtin.domainId = (uint32_t)boost::interprocess::ipcdetail::get_current_process_id() % 230;
        participant_ = eprosima::fastrtps::Domain::createParticipant(participant_attr_);

        if(participant_ != nullptr)
        {
            // Register type
            eprosima::fastrtps::Domain::registerType(participant_, &type_);

            //Create publisher
            publisher_ = eprosima::fastrtps::Domain::createPublisher(participant_, publisher_attr_, &listener_);

            if(publisher_ != nullptr)
            {
                initialized_ = true;
                return;
            }

            eprosima::fastrtps::Domain::removeParticipant(participant_);
        }
    }

    bool isInitialized() const { return initialized_; }

    eprosima::fastrtps::Participant* getParticipant()
    {
        return participant_;
    }

    void destroy()
    {
        if(participant_ != nullptr)
        {
            eprosima::fastrtps::Domain::removeParticipant(participant_);
            participant_ = nullptr;
        }
    }

    void send(std::list<type>& msgs)
    {
        auto it = msgs.begin();

        while(it != msgs.end())
        {
            if(publisher_->write((void*)&(*it)))
            {
                it = msgs.erase(it);
            }
            else
                break;
        }
    }

    void waitDiscovery()
    {
        std::cout << "Writer waiting for discovery..." << std::endl;
        std::unique_lock<std::mutex> lock(mutex_);

        if(matched_ == 0)
            cv_.wait_for(lock, std::chrono::seconds(10));

        ASSERT_NE(matched_, 0u);
        std::cout << "Writer discovery phase finished" << std::endl;
    }

    void waitRemoval()
    {
        std::unique_lock<std::mutex> lock(mutex_);

        if(matched_ != 0)
            cv_.wait_for(lock, std::chrono::seconds(10));

        ASSERT_EQ(matched_, 0u);
    }

    template<class _Rep,
        class _Period
            >
            bool waitForAllAcked(const std::chrono::duration<_Rep, _Period>& max_wait)
            {
                return publisher_->wait_for_all_acked(Time_t((int32_t)max_wait.count(), 0));
            }

    /*** Function to change QoS ***/
    PubSubWriter& reliability(const eprosima::fastrtps::ReliabilityQosPolicyKind kind)
    {
        publisher_attr_.qos.m_reliability.kind = kind;
        return *this;
    }

    PubSubWriter& add_throughput_controller_descriptor_to_pparams(uint32_t bytesPerPeriod, uint32_t periodInMs)
    {
        ThroughputControllerDescriptor descriptor {bytesPerPeriod, periodInMs};
        participant_attr_.rtps.throughputController = descriptor;

        return *this;
    }

    PubSubWriter& asynchronously(const eprosima::fastrtps::PublishModeQosPolicyKind kind)
    {
        publisher_attr_.qos.m_publishMode.kind = kind;
        return *this;
    }

    PubSubWriter& history_kind(const eprosima::fastrtps::HistoryQosPolicyKind kind)
    {
        publisher_attr_.topic.historyQos.kind = kind;
        return *this;
    }

    PubSubWriter& history_depth(const int32_t depth)
    {
        publisher_attr_.topic.historyQos.depth = depth;
        return *this;
    }

    PubSubWriter& disable_builtin_transport()
    {
        participant_attr_.rtps.useBuiltinTransports = false;
        return *this;
    }

    PubSubWriter& add_user_transport_to_pparams(std::shared_ptr<TransportDescriptorInterface> userTransportDescriptor)
    {
        participant_attr_.rtps.userTransports.push_back(userTransportDescriptor);
        return *this;
    }

    PubSubWriter& durability_kind(const eprosima::fastrtps::DurabilityQosPolicyKind kind)
    {
        publisher_attr_.qos.m_durability.kind = kind;
        return *this;
    }

    PubSubWriter& resource_limits_allocated_samples(const int32_t initial)
    {
        publisher_attr_.topic.resourceLimitsQos.allocated_samples = initial;
        return *this;
    }

    PubSubWriter& resource_limits_max_samples(const int32_t max)
    {
        publisher_attr_.topic.resourceLimitsQos.max_samples = max;
        return *this;
    }

    PubSubWriter& heartbeat_period_seconds(int32_t sec)
    {
        publisher_attr_.times.heartbeatPeriod.seconds = sec;
        return *this;
    }

    PubSubWriter& heartbeat_period_fraction(uint32_t frac)
    {
        publisher_attr_.times.heartbeatPeriod.fraction = frac;
        return *this;
    }

    PubSubWriter& unicastLocatorList(LocatorList_t unicastLocators)
    {
        publisher_attr_.unicastLocatorList = unicastLocators;
        return *this;
    }

    PubSubWriter& multicastLocatorList(LocatorList_t multicastLocators)
    {
        publisher_attr_.multicastLocatorList = multicastLocators;
        return *this;
    }

    PubSubWriter& outLocatorList(LocatorList_t outLocators)
    {
        publisher_attr_.outLocatorList = outLocators;
        return *this;
    }

    PubSubWriter& static_discovery(const char* filename)
    {
        participant_attr_.rtps.builtin.use_SIMPLE_EndpointDiscoveryProtocol = false;
        participant_attr_.rtps.builtin.use_STATIC_EndpointDiscoveryProtocol = true;
        participant_attr_.rtps.builtin.setStaticEndpointXMLFilename(filename);
        return *this;
    }

    PubSubWriter& setPublisherIDs(uint8_t UserID, uint8_t EntityID)
    {
        publisher_attr_.setUserDefinedID(UserID);
        publisher_attr_.setEntityID(EntityID);
        return *this;
    }

    PubSubWriter& setManualTopicName(std::string topicName)
    {
        publisher_attr_.topic.topicName=topicName;
        return *this;
    }
    private:

    void matched()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        ++matched_;
        cv_.notify_one();
    }

    void unmatched()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        --matched_;
        cv_.notify_one();
    }

    PubSubWriter& operator=(const PubSubWriter&)NON_COPYABLE_CXX11;

    eprosima::fastrtps::Participant *participant_;
    eprosima::fastrtps::PublisherAttributes publisher_attr_;
    eprosima::fastrtps::ParticipantAttributes participant_attr_;
    eprosima::fastrtps::Publisher *publisher_;
    std::string topic_name_;
    bool initialized_;
    std::mutex mutex_;
    std::condition_variable cv_;
    unsigned int matched_;
    type_support type_;
};

#endif // _TEST_BLACKBOX_PUBSUBWRITER_HPP_
