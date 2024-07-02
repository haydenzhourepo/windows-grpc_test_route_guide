#include <random>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>
#include <vector>

#include <grpc/grpc.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>
#include "route_guide.grpc.pb.h"

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerReader;
using grpc::ServerReaderWriter;
using grpc::ServerWriter;
using grpc::Status;
using routeguide::Feature;
using routeguide::Point;
using routeguide::Rectangle;
using routeguide::RouteGuide;
using routeguide::RouteNote;
using routeguide::RouteSummary;
using std::chrono::system_clock;


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

float ConvertToRadians(float num) { return num * 3.1415926 / 180; }

// The formula is based on http://mathforum.org/library/drmath/view/51879.html
float GetDistance(const Point& start, const Point& end) {
    const float kCoordFactor = 10000000.0;
    float lat_1 = start.latitude() / kCoordFactor;
    float lat_2 = end.latitude() / kCoordFactor;
    float lon_1 = start.longitude() / kCoordFactor;
    float lon_2 = end.longitude() / kCoordFactor;
    float lat_rad_1 = ConvertToRadians(lat_1);
    float lat_rad_2 = ConvertToRadians(lat_2);
    float delta_lat_rad = ConvertToRadians(lat_2 - lat_1);
    float delta_lon_rad = ConvertToRadians(lon_2 - lon_1);

    float a = pow(sin(delta_lat_rad / 2), 2) +
        cos(lat_rad_1) * cos(lat_rad_2) * pow(sin(delta_lon_rad / 2), 2);
    float c = 2 * atan2(sqrt(a), sqrt(1 - a));
    int R = 6371000;  // metres

    return R * c;
}

std::string GetFeatureName(const Point& point,
    const std::vector<Feature>& feature_list) {
    for (const Feature& f : feature_list) {
        if (f.location().latitude() == point.latitude() &&
            f.location().longitude() == point.longitude()) {
            return f.name();
        }
    }
    return "";
}

class RouteGuideImpl final : public RouteGuide::Service {
public:
    explicit RouteGuideImpl(const std::string& db) {
        routeguide::ParseDb(db, &feature_list_);
    }

    Status GetFeature(ServerContext* context, const Point* point,
        Feature* feature) override {
        feature->set_name(GetFeatureName(*point, feature_list_));
        feature->mutable_location()->CopyFrom(*point);
        return Status::OK;
    }

    Status ListFeatures(ServerContext* context,
        const routeguide::Rectangle* rectangle,
        ServerWriter<Feature>* writer) override {
        auto lo = rectangle->lo();
        auto hi = rectangle->hi();
        long left = (std::min)(lo.longitude(), hi.longitude());
        long right = (std::max)(lo.longitude(), hi.longitude());
        long top = (std::max)(lo.latitude(), hi.latitude());
        long bottom = (std::min)(lo.latitude(), hi.latitude());
        for (const Feature& f : feature_list_) {
            if (f.location().longitude() >= left &&
                f.location().longitude() <= right &&
                f.location().latitude() >= bottom && f.location().latitude() <= top) {
                writer->Write(f);
            }
        }
        return Status::OK;
    }

    Status RecordRoute(ServerContext* context, ServerReader<Point>* reader,
        RouteSummary* summary) override {
        Point point;
        int point_count = 0;
        int feature_count = 0;
        float distance = 0.0;
        Point previous;

        system_clock::time_point start_time = system_clock::now();
        while (reader->Read(&point)) {
            point_count++;
            if (!GetFeatureName(point, feature_list_).empty()) {
                feature_count++;
            }
            if (point_count != 1) {
                distance += GetDistance(previous, point);
            }
            previous = point;
        }
        system_clock::time_point end_time = system_clock::now();
        summary->set_point_count(point_count);
        summary->set_feature_count(feature_count);
        summary->set_distance(static_cast<long>(distance));
        auto secs =
            std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time);
        summary->set_elapsed_time(secs.count());

        return Status::OK;
    }

    Status RouteChat(ServerContext* context,
        ServerReaderWriter<RouteNote, RouteNote>* stream) override {
        RouteNote note;
        while (stream->Read(&note)) {
            std::unique_lock<std::mutex> lock(mu_);
            for (const RouteNote& n : received_notes_) {
                if (n.location().latitude() == note.location().latitude() &&
                    n.location().longitude() == note.location().longitude()) {
                    stream->Write(n);
                }
            }
            received_notes_.push_back(note);
        }

        return Status::OK;
    }

private:
    std::vector<Feature> feature_list_;
    std::mutex mu_;
    std::vector<RouteNote> received_notes_;
};

void RunServer(const std::string& db_path) {
    std::string server_address("0.0.0.0:50051");
    RouteGuideImpl service(db_path);

    ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    std::unique_ptr<Server> server(builder.BuildAndStart());
    std::cout << "Server listening on " << server_address << std::endl;
    server->Wait();
}

int main(int argc, char** argv) {
    // Expect only arg: --db_path=path/to/route_guide_db.json.
    std::string db = routeguide::GetDbFileContent(argc, argv);
    RunServer(db);

    return 0;
}