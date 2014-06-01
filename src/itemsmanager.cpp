/*
    Copyright 2014 Ilya Zhuravlev

    This file is part of Acquisition.

    Acquisition is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Acquisition is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Acquisition.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "itemsmanager.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSignalMapper>
#include <QTimer>
#include <QUrlQuery>
#include "jsoncpp/json.h"
#include <iostream>
#include <stdexcept>

#include "mainwindow.h"
#include "datamanager.h"

const char *POE_STASH_URL = "http://www.pathofexile.com/character-window/get-stash-items";

ItemsManager::ItemsManager(MainWindow *app):
    app_(app),
    signal_mapper_(new QSignalMapper)
{
}

void ItemsManager::Init() {
    LoadSavedData();
}

QNetworkRequest ItemsManager::MakeRequest(int tab_index, bool tabs) {
    QUrlQuery query;
    query.addQueryItem("league", app_->league().c_str());
    query.addQueryItem("tabs", tabs ? "1" : "0");
    query.addQueryItem("tabIndex", QString::number(tab_index));

    QUrl url(POE_STASH_URL);
    url.setQuery(query);
    return QNetworkRequest(url);
}

void ItemsManager::Update() {
    // remove all mappings (from previous requests)
    delete signal_mapper_;
    signal_mapper_ = new QSignalMapper;
    // remove all pending requests
    tabs_queue_ = std::queue<int>();
    for (auto &reply : replies_)
        delete reply.second;
    replies_.clear();
    items_as_json_.clear();
    items_.clear();

    // first step, fetch first tab and get list of all tabs
    QNetworkReply *first_tab = app_->logged_in_nm()->get(MakeRequest(0, true));
    connect(first_tab, SIGNAL(finished()), this, SLOT(OnFirstTabReceived()));
}

void ItemsManager::FetchSomeTabs(int limit) {
    std::cout << "fetchsometabs" << std::endl;
    int count = std::min(limit, static_cast<int>(tabs_queue_.size()));
    for (int i = 0; i < count; ++i) {
        int index = tabs_queue_.front();
        tabs_queue_.pop();

        std::cout << "Requesting tab " << index << std::endl;

        QNetworkReply *tab_fetched = app_->logged_in_nm()->get(MakeRequest(index, false));
        signal_mapper_->setMapping(tab_fetched, index);
        connect(tab_fetched, SIGNAL(finished()), signal_mapper_, SLOT(map()));
        replies_[index] = tab_fetched;
    }
    requests_needed_ = count;
    requests_completed_ = 0;
}

void ItemsManager::OnFirstTabReceived() {
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(QObject::sender());
    QByteArray bytes = reply->readAll();

    std::string json(bytes.constData(), bytes.size());
    Json::Value root;
    Json::Reader reader;
    reader.parse(json, root);

    int index = 0;
    if (!root.isObject())
        throw std::runtime_error("First response is not an object.");
    tabs_.clear();
    tabs_as_json_ = root["tabs"];
    for (auto tab : root["tabs"]) {
        tabs_.push_back(tab["n"].asString());
        if (index > 0)
            tabs_queue_.push(index);
        ++index;
    }
    ParseItems(root, 0);
    tabs_needed_ = tabs_.size();
    tabs_received_ = 1;
    FetchSomeTabs(THROTTLE_REQUESTS - 1);

    connect(signal_mapper_, SIGNAL(mapped(int)), this, SLOT(OnTabReceived(int)));
}

void ItemsManager::ParseItems(const Json::Value &root, int tab) {
    for (auto item : root["items"]) {
        item["_tab"] = tab;
        item["_tab_label"] = tabs_[tab];
        items_as_json_.append(item);
        items_.push_back(std::make_shared<Item>(item, tab, tabs_[tab]));
    }
}

void ItemsManager::LoadSavedData() {
    items_.clear();
    std::string items = app_->data_manager()->Get("items");
    if (items.size() != 0) {
        Json::Value root;
        Json::Reader reader;
        reader.parse(items, root);
        for (auto &item : root) {
            items_.push_back(std::make_shared<Item>(item, item["_tab"].asInt(), item["_tab_label"].asString()));
        }
    }

    tabs_.clear();
    std::string tabs = app_->data_manager()->Get("tabs");
    if (tabs.size() != 0) {
        Json::Value root;
        Json::Reader reader;
        reader.parse(tabs, root);
        for (auto &tab : root)
            tabs_.push_back(tab["n"].asString());
    }
    emit ItemsRefreshed(items_, tabs_);
}

void ItemsManager::OnTabReceived(int index) {
    if (!replies_.count(index)) {
        std::cout << "WARN: received a tab (" << index << ") that was not requested." << std::endl;
        return;
    }
    ++requests_completed_;
    if (requests_completed_ == requests_needed_ && tabs_queue_.size() > 0) {
        emit StatusUpdate(tabs_received_ + 1, tabs_needed_, true);
        std::cout << "Sleeping one minute to prevent throttling." << std::endl;
        QTimer::singleShot(THROTTLE_SLEEP * 1000, this, SLOT(FetchSomeTabs()));
    } else {
        emit StatusUpdate(tabs_received_ + 1, tabs_needed_, false);
    }

    QNetworkReply *reply = replies_[index];
    QByteArray bytes = reply->readAll();
    std::string json(bytes.constData(), bytes.size());
    Json::Value root;
    Json::Reader reader;
    reader.parse(json, root);

    if (root.isMember("error")) {
        std::cout << index << " WARN: got 'error' instead of stash tab contents, this shouldn't normally happen." << std::endl;
        // but it just happened
        tabs_queue_.push(index);
        return;
    }

    ParseItems(root, index);

    ++tabs_received_;
    std::cout << tabs_received_ << "/" << tabs_needed_ << std::endl;
    if (tabs_received_ == tabs_needed_) {
        // all tabs were received
        emit ItemsRefreshed(items_, tabs_);

        Json::FastWriter writer;
        app_->data_manager()->Set("items", writer.write(items_as_json_));
        app_->data_manager()->Set("tabs", writer.write(tabs_as_json_));
    }
}
