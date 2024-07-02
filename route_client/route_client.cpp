/*
 *
 * Copyright 2015 gRPC authors.
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
 *
 */

#include <chrono>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <fstream>
#include <sstream>


#include <grpc/grpc.h>
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>
#ifdef BAZEL_BUILD
#include "examples/protos/route_guide.grpc.pb.h"
#else
#include "route_guide.grpc.pb.h"
#endif


namespace routeguide {

    std::string GetDbFileContent(int argc, char** argv) {
        std::string db_path;
        std::string arg_str("--db_path");
        if (argc > 1) {
            std::string argv_1 = argv[1];
            size_t start_position = argv_1.find(arg_str);
            if (start_position != std::string::npos) {
                start_position += arg_str.size();
                if (argv_1[start_position] == ' ' || argv_1[start_position] == '=') {
                    db_path = argv_1.substr(start_position + 1);
                }
            }
        }
        else {
#ifdef BAZEL_BUILD
            db_path = "cpp/route_guide/route_guide_db.json";
#else
            db_path = "route_guide_db.json";
#endif
        }
        std::ifstream db_file(db_path);
        if (!db_file.is_open()) {
            std::cout << "Failed to open " << db_path << std::endl;
            abort();
        }
        std::stringstream db;
        db << db_file.rdbuf();
        return db.str();
    }

    // A simple parser for the json db file. It requires the db file to have the
    // exact form of [{"location": { "latitude": 123, "longitude": 456}, "name":
    // "the name can be empty" }, { ... } ... The spaces will be stripped.
    class Parser {
    public:
        explicit Parser(const std::string& db) : db_(db) {
            // Remove all spaces.
            db_.erase(std::remove_if(db_.begin(), db_.end(), isspace), db_.end());
            if (!Match("[")) {
                SetFailedAndReturnFalse();
            }
        }

        bool Finished() { return current_ >= db_.size(); }

        bool TryParseOne(Feature* feature) {
            if (failed_ || Finished() || !Match("{")) {
                return SetFailedAndReturnFalse();
            }
            if (!Match(location_) || !Match("{") || !Match(latitude_)) {
                return SetFailedAndReturnFalse();
            }
            long temp = 0;
            ReadLong(&temp);
            feature->mutable_location()->set_latitude(temp);
            if (!Match(",") || !Match(longitude_)) {
                return SetFailedAndReturnFalse();
            }
            ReadLong(&temp);
            feature->mutable_location()->set_longitude(temp);
            if (!Match("},") || !Match(name_) || !Match("\"")) {
                return SetFailedAndReturnFalse();
            }
            size_t name_start = current_;
            while (current_ != db_.size() && db_[current_++] != '"') {
            }
            if (current_ == db_.size()) {
                return SetFailedAndReturnFalse();
            }
            feature->set_name(db_.substr(name_start, current_ - name_start - 1));
            if (!Match("},")) {
                if (db_[current_ - 1] == ']' && current_ == db_.size()) {
                    return true;
                }
                return SetFailedAndReturnFalse();
            }
            return true;
        }

    private:
        bool SetFailedAndReturnFalse() {
            failed_ = true;
            return false;
        }

        bool Match(const std::string& prefix) {
            bool eq = db_.substr(current_, prefix.size()) == prefix;
            current_ += prefix.size();
            return eq;
        }

        void ReadLong(long* l) {
            size_t start = current_;
            while (current_ != db_.size() && db_[current_] != ',' &&
                db_[current_] != '}') {
                current_++;
            }
            // It will throw an exception if fails.
            *l = std::stol(db_.substr(start, current_ - start));
        }

        bool failed_ = false;
        std::string db_;
        size_t current_ = 0;
        const std::string location_ = "\"location\":";
        const std::string latitude_ = "\"latitude\":";
        const std::string longitude_ = "\"longitude\":";
        const std::string name_ = "\"name\":";
    };

    void ParseDb(const std::string& db, std::vector<Feature>* feature_list) {
        feature_list->clear();
        std::string db_content(db);
        db_content.erase(
            std::remove_if(db_content.begin(), db_content.end(), isspace),
            db_content.end());

        Parser parser(db_content);
        Feature feature;
        while (!parser.Finished()) {
            feature_list->push_back(Feature());
            if (!parser.TryParseOne(&feature_list->back())) {
                std::cout << "Error parsing the db file";
                feature_list->clear();
                break;
            }
        }
        std::cout << "DB parsed, loaded " << feature_list->size() << " features."
            << std::endl;
    }

}  

using grpc::Channel;
using grpc::ClientContext;
using grpc::ClientReader;
using grpc::ClientReaderWriter;
using grpc::ClientWriter;
using grpc::Status;
using routeguide::Feature;
using routeguide::Point;
using routeguide::Rectangle;
using routeguide::RouteGuide;
using routeguide::RouteNote;
using routeguide::RouteSummary;

Point MakePoint(long latitude, long longitude) {
    Point p;
    p.set_latitude(latitude);
    p.set_longitude(longitude);
    return p;
}

Feature MakeFeature(const std::string& name, long latitude, long longitude) {
    Feature f;
    f.set_name(name);
    f.mutable_location()->CopyFrom(MakePoint(latitude, longitude));
    return f;
}

RouteNote MakeRouteNote(const std::string& message, long latitude,
    long longitude) {
    RouteNote n;
    n.set_message(message);
    n.mutable_location()->CopyFrom(MakePoint(latitude, longitude));
    return n;
}

class RouteGuideClient {
public:
    RouteGuideClient(std::shared_ptr<Channel> channel, const std::string& db)
        : stub_(RouteGuide::NewStub(channel)) {
        routeguide::ParseDb(db, &feature_list_);
    }

    void GetFeature() {
        Point point;
        Feature feature;
        point = MakePoint(409146138, -746188906);
        GetOneFeature(point, &feature);
        point = MakePoint(0, 0);
        GetOneFeature(point, &feature);
    }

    void ListFeatures() {
        routeguide::Rectangle rect;
        Feature feature;
        ClientContext context;

        rect.mutable_lo()->set_latitude(400000000);
        rect.mutable_lo()->set_longitude(-750000000);
        rect.mutable_hi()->set_latitude(420000000);
        rect.mutable_hi()->set_longitude(-730000000);
        std::cout << "Looking for features between 40, -75 and 42, -73"
            << std::endl;

        std::unique_ptr<ClientReader<Feature> > reader(
            stub_->ListFeatures(&context, rect));
        while (reader->Read(&feature)) {
            std::cout << "Found feature called " << feature.name() << " at "
                << feature.location().latitude() / kCoordFactor_ << ", "
                << feature.location().longitude() / kCoordFactor_ << std::endl;
        }
        Status status = reader->Finish();
        if (status.ok()) {
            std::cout << "ListFeatures rpc succeeded." << std::endl;
        }
        else {
            std::cout << "ListFeatures rpc failed." << std::endl;
        }
    }

    void RecordRoute() {
        Point point;
        RouteSummary stats;
        ClientContext context;
        const int kPoints = 10;
        unsigned seed = std::chrono::system_clock::now().time_since_epoch().count();

        std::default_random_engine generator(seed);
        std::uniform_int_distribution<int> feature_distribution(
            0, feature_list_.size() - 1);
        std::uniform_int_distribution<int> delay_distribution(500, 1500);

        std::unique_ptr<ClientWriter<Point> > writer(
            stub_->RecordRoute(&context, &stats));
        for (int i = 0; i < kPoints; i++) {
            const Feature& f = feature_list_[feature_distribution(generator)];
            std::cout << "Visiting point " << f.location().latitude() / kCoordFactor_
                << ", " << f.location().longitude() / kCoordFactor_
                << std::endl;
            if (!writer->Write(f.location())) {
                // Broken stream.
                break;
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(delay_distribution(generator)));
        }
        writer->WritesDone();
        Status status = writer->Finish();
        if (status.ok()) {
            std::cout << "Finished trip with " << stats.point_count() << " points\n"
                << "Passed " << stats.feature_count() << " features\n"
                << "Travelled " << stats.distance() << " meters\n"
                << "It took " << stats.elapsed_time() << " seconds"
                << std::endl;
        }
        else {
            std::cout << "RecordRoute rpc failed." << std::endl;
        }
    }

    void RouteChat() {
        ClientContext context;

        std::shared_ptr<ClientReaderWriter<RouteNote, RouteNote> > stream(
            stub_->RouteChat(&context));

        std::thread writer([stream]() {
            std::vector<RouteNote> notes{ MakeRouteNote("First message", 0, 0),
                                         MakeRouteNote("Second message", 0, 1),
                                         MakeRouteNote("Third message", 1, 0),
                                         MakeRouteNote("Fourth message", 0, 0) };
        for (const RouteNote& note : notes) {
            std::cout << "Sending message " << note.message() << " at "
                << note.location().latitude() << ", "
                << note.location().longitude() << std::endl;
            stream->Write(note);
        }
        stream->WritesDone();
            });

        RouteNote server_note;
        while (stream->Read(&server_note)) {
            std::cout << "Got message " << server_note.message() << " at "
                << server_note.location().latitude() << ", "
                << server_note.location().longitude() << std::endl;
        }
        writer.join();
        Status status = stream->Finish();
        if (!status.ok()) {
            std::cout << "RouteChat rpc failed." << std::endl;
        }
    }

private:
    bool GetOneFeature(const Point& point, Feature* feature) {
        ClientContext context;
        Status status = stub_->GetFeature(&context, point, feature);
        if (!status.ok()) {
            std::cout << "GetFeature rpc failed." << std::endl;
            return false;
        }
        if (!feature->has_location()) {
            std::cout << "Server returns incomplete feature." << std::endl;
            return false;
        }
        if (feature->name().empty()) {
            std::cout << "Found no feature at "
                << feature->location().latitude() / kCoordFactor_ << ", "
                << feature->location().longitude() / kCoordFactor_ << std::endl;
        }
        else {
            std::cout << "Found feature called " << feature->name() << " at "
                << feature->location().latitude() / kCoordFactor_ << ", "
                << feature->location().longitude() / kCoordFactor_ << std::endl;
        }
        return true;
    }

    const float kCoordFactor_ = 10000000.0;
    std::unique_ptr<RouteGuide::Stub> stub_;
    std::vector<Feature> feature_list_;
};

int main(int argc, char** argv) {
    // Expect only arg: --db_path=path/to/route_guide_db.json.
    std::string db = routeguide::GetDbFileContent(argc, argv);
    RouteGuideClient guide(
        grpc::CreateChannel("localhost:50051",
            grpc::InsecureChannelCredentials()),
        db);

    std::cout << "-------------- GetFeature --------------" << std::endl;
    guide.GetFeature();
    std::cout << "-------------- ListFeatures --------------" << std::endl;
    guide.ListFeatures();
    std::cout << "-------------- RecordRoute --------------" << std::endl;
    guide.RecordRoute();
    std::cout << "-------------- RouteChat --------------" << std::endl;
    guide.RouteChat();

    return 0;
}