// Shared by full Library and the Designer's existing single LibraryView instance.
void LibraryView::rebuildNavigation()
{
    juce::ScopedValueSetter<bool> guard(updating, true);
    int totalSources = 0;
    for (auto &r : snapshot.roots)
        totalSources += r.discovered;
    navigationRows = {{"scope", {}, "All Sounds", 0, totalSources, false},
                      {"scope", "favorites", "Favorites", 0, snapshot.organization.favorites, false},
                      {"scope", "recent", "Recently Used", 0, snapshot.organization.recent, false},
                      {"heading", "collections", "My Collections", 0, 0, false}};
    std::function<void(const juce::String &, int)> visit = [&](const auto &parent, int depth)
    {
        for (auto &c : snapshot.organization.nodes)
            if (c.parent == parent)
            {
                bool children =
                    std::any_of(snapshot.organization.nodes.begin(), snapshot.organization.nodes.end(),
                                [&](auto &n) { return n.parent == c.id; });
                navigationRows.push_back({"scope", c.id, c.name, depth, c.members, children});
                if (children && !collapsedCollections.contains(c.id))
                    visit(c.id, depth + 1);
            }
    };
    visit({}, 0);
    navigationRows.push_back({"heading", {}, "Sources", 0, 0, false});
    navigationRows.push_back({"source", {}, "All sources", 0, totalSources, false});
    for (auto &r : snapshot.roots)
        navigationRows.push_back({"source", r.id,
                                  juce::File(r.path).getFileName() + (r.online ? "" : " · Offline"), 0,
                                  r.discovered, false});
    sources.updateContent();
    sources.repaint();
    updateScope();
}
juce::String LibraryView::navigationName(int n) const
{
    if (!juce::isPositiveAndBelow(n, (int)navigationRows.size()))
        return {};
    auto &r = navigationRows[(size_t)n];
    if (r.kind == "heading")
        return r.name + ", group";
    return r.name + ", level " + juce::String(r.depth + 1) +
           (r.children ? (collapsedCollections.contains(r.id) ? ", collapsed" : ", expanded") : "") + ", " +
           juce::String(r.count) + " direct items";
}
void LibraryView::chooseNavigation(int n)
{
    if (!juce::isPositiveAndBelow(n, (int)navigationRows.size()))
        return;
    auto &r = navigationRows[(size_t)n];
    if (r.kind == "scope")
        selectCollectionScope(r.id);
    else if (r.kind == "source" && rootFilter != r.id)
    {
        rootFilter = r.id;
        preserveScopeSelection = true;
        updateScope();
        requestSearch();
    }
}
void LibraryView::selectCollectionScope(const juce::String &id)
{
    collectionScope = id;
    preserveScopeSelection = true;
    bool reveal = false;
    auto cursor = id;
    for (int depth = 0; cursor.isNotEmpty() && depth < 16; ++depth)
    {
        auto found = std::find_if(snapshot.organization.nodes.begin(), snapshot.organization.nodes.end(),
                                  [&](const auto &c) { return c.id == cursor; });
        if (found == snapshot.organization.nodes.end())
            break;
        cursor = found->parent;
        reveal |= collapsedCollections.erase(cursor) > 0;
    }
    if (preferences)
    {
        preferences->setValue("library.collectionScope", id);
        if (reveal)
        {
            juce::StringArray collapsed;
            for (const auto &key : collapsedCollections)
                collapsed.add(key);
            preferences->setValue("library.collapsedCollections", collapsed.joinIntoString(","));
        }
    }
    if (reveal)
        rebuildNavigation();
    updateScope();
    requestSearch();
    sources.repaint();
}
BookmarkRequest LibraryView::currentBookmark() const
{
    if (!selectedAsset)
        return {};
    auto a = *selectedAsset;
    if (!a.deliverable() && a.libraryReference.isNotEmpty())
        return {a, a.bookmarkFirst, a.bookmarkLast};
    bool range = regionSelected || a.bookmarkFirst >= 0;
    return {a, range ? regionStart : -1, range ? regionEnd : -1};
}
juce::String LibraryView::currentBookmarkKey() const
{
    auto b = currentBookmark();
    return b.asset.id + ":" + juce::String(b.asset.revision) + ":" + juce::String(b.first) + ":" +
           juce::String(b.last);
}
std::vector<BookmarkRequest> LibraryView::selectedBookmarks() const
{
    std::vector<BookmarkRequest> items;
    auto rows = table.getSelectedRows();
    for (int i = 0; i < rows.size(); ++i)
        if (auto *a = rowAsset(rows[i]))
        {
            if (a->resultKey() == selectedKey)
                items.push_back(currentBookmark());
            else
                items.push_back({*a, a->bookmarkFirst, a->bookmarkLast});
        }
    if (items.size() != (size_t)rows.size())
        return {};
    if (items.empty() && selectedAsset)
        items.push_back(currentBookmark());
    return items;
}
void LibraryView::inspectFavorite()
{
    if (!selectedAsset)
        return;
    auto key = currentBookmarkKey();
    if (key == favoriteInspectionKey)
        return;
    favoriteInspectionKey = key;
    library.inspectBookmark(currentBookmark(), key);
}
void LibraryView::submitCollectionEdit(CollectionEdit edit)
{
    if ((edit.action == "favorite" || edit.action == "member") && edit.items.empty())
    {
        organizationUiError = "Select sounds within one loaded page (up to 512) for a bulk action.";
        return;
    }
    organizationUiError.clear();
    preserveScopeSelection = true;
    favoriteInspectionKey.clear();
    const auto created = edit.action == "create" ? edit.id : juce::String();
    if (library.editCollections(std::move(edit)) && created.isNotEmpty())
        newCollectionScope = created;
    juce::Component::SafePointer<LibraryView> safe(this);
    juce::MessageManager::callAsync(
        [safe]
        {
            if (safe && safe->isShowing() && !safe->hasKeyboardFocus(true))
                safe->collectionActions.grabKeyboardFocus();
        });
}
std::vector<std::pair<juce::String, juce::String>> LibraryView::collectionChoices() const
{
    std::map<juce::String, std::vector<const LibraryCollection *>> children;
    for (const auto &c : snapshot.organization.nodes)
        children[c.parent].push_back(&c);
    std::vector<std::pair<juce::String, juce::String>> choices;
    const auto visit = [&](const auto &self, const juce::String &parent, const juce::String &prefix, int depth) -> void
    {
        auto node = children.find(parent);
        if (depth == 16 || node == children.end())
            return;
        for (const auto *child : node->second)
        {
            auto path = prefix + child->name;
            choices.emplace_back(child->id, path);
            self(self, child->id, path + " / ", depth + 1);
        }
    };
    visit(visit, {}, {}, 0);
    return choices;
}
void LibraryView::nameCollection(bool rename, const juce::String &node, std::vector<BookmarkRequest> items)
{
    juce::String initial;
    if (rename)
        for (auto &c : snapshot.organization.nodes)
            if (c.id == node)
                initial = c.name;
    juce::String message;
    if (items.size() == 1)
        message = "Add " + items.front().asset.name;
    else if (!items.empty())
        message = "Add " + juce::String((int)items.size()) + " recordings";
    auto *dialog = new juce::AlertWindow(rename ? "Rename Collection" : "New Collection",
                                         message, juce::MessageBoxIconType::NoIcon, this);
    dialog->addTextEditor("name", initial, "Name");
    juce::StringArray parentIds{juce::String()};
    if (!rename)
    {
        juce::StringArray parentNames{"My Collections"};
        for (const auto &[id, path] : collectionChoices())
        {
            parentIds.add(id);
            parentNames.add(path);
        }
        dialog->addComboBox("parent", parentNames, "Parent");
        dialog->getComboBoxComponent("parent")->setSelectedItemIndex(std::max(0, parentIds.indexOf(node)));
    }
    dialog->addButton(rename ? "Rename" : "Create", 1, juce::KeyPress(juce::KeyPress::returnKey));
    dialog->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<LibraryView> safe(this);
    dialog->enterModalState(true,
                            juce::ModalCallbackFunction::create(
                                [safe, dialog, rename, node, parents = std::move(parentIds),
                                 items = std::move(items)](int result) mutable
                                {
                                    if (result == 1 && safe)
                                    {
                                        CollectionEdit e;
                                        e.action = rename ? "rename" : "create";
                                        e.id = rename ? node : juce::Uuid().toString().removeCharacters("-");
                                        if (!rename)
                                            e.parent = parents[dialog->getComboBoxComponent("parent")
                                                                   ->getSelectedItemIndex()];
                                        e.name = dialog->getTextEditorContents("name");
                                        e.position = 1024;
                                        e.items = std::move(items);
                                        safe->submitCollectionEdit(std::move(e));
                                    }
                                }),
                            true);
    dialog->getTextEditor("name")->grabKeyboardFocus();
}
void LibraryView::addCollectionDestinations(juce::PopupMenu &menu, const std::vector<BookmarkRequest> &items)
{
    const auto safe = juce::Component::SafePointer<LibraryView>(this);
    const auto selection = std::make_shared<const std::vector<BookmarkRequest>>(items);
    const auto parent = collectionScope;
    menu.addItem(juce::String::fromUTF8("New Collection"), !items.empty(), false,
                 [safe, selection, parent] { if (safe) safe->nameCollection(false, parent, *selection); });
    if (!snapshot.organization.nodes.empty())
        menu.addSeparator();
    for (const auto &[id, path] : collectionChoices())
        menu.addItem(path, !items.empty(), false, [safe, selection, id]
        {
            if (!safe)
                return;
            CollectionEdit e;
            e.action = "member";
            e.id = id;
            e.items = *selection;
            safe->submitCollectionEdit(std::move(e));
        });
}
void LibraryView::showAddToCollection()
{
    juce::PopupMenu menu;
    addCollectionDestinations(menu, selectedBookmarks());
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&addToCollection).withParentComponent(this));
}
void LibraryView::collectionMenu(bool selection, const juce::String &node, juce::Rectangle<int> menuAnchor)
{
    juce::PopupMenu menu;
    std::map<int, std::function<void()>> actions;
    int nextId = 1;
    juce::Component::SafePointer<LibraryView> safe(this);
    auto add = [&](juce::PopupMenu &m, const juce::String &label, std::function<void()> callback,
                   bool enabled = true, bool ticked = false)
    {
        int id = nextId++;
        m.addItem(id, label, enabled, ticked);
        actions[id] = std::move(callback);
    };
    if (node.isNotEmpty())
    {
        auto found = std::find_if(snapshot.organization.nodes.begin(), snapshot.organization.nodes.end(),
                                  [&](auto &n) { return n.id == node; });
        if (found == snapshot.organization.nodes.end())
            return;
        auto current = *found;
        add(menu, "Open Collection",
            [safe, node]
            {
                if (safe)
                    safe->selectCollectionScope(node);
            });
        add(menu, juce::String::fromUTF8("New Subcollection"),
            [safe, node]
            {
                if (safe)
                    safe->nameCollection(false, node);
            });
        add(menu, juce::String::fromUTF8("Rename"),
            [safe, node]
            {
                if (safe)
                    safe->nameCollection(true, node);
            });
        juce::PopupMenu move;
        auto destination = [&](const juce::String &id, const juce::String &name)
        {
            add(move, name,
                [safe, node, id]
                {
                    if (safe)
                    {
                        CollectionEdit e;
                        e.action = "move";
                        e.id = node;
                        e.parent = id;
                        e.position = 1024;
                        safe->submitCollectionEdit(e);
                    }
                });
        };
        if (current.parent.isNotEmpty())
            destination({}, "My Collections");
        for (const auto &[id, path] : collectionChoices())
        {
            auto cursor = id;
            while (cursor.isNotEmpty() && cursor != node)
            {
                auto ancestor = std::find_if(snapshot.organization.nodes.begin(), snapshot.organization.nodes.end(),
                                             [&](const auto &entry) { return entry.id == cursor; });
                cursor = ancestor == snapshot.organization.nodes.end() ? juce::String() : ancestor->parent;
            }
            if (cursor != node && id != current.parent)
                destination(id, path);
        }
        menu.addSubMenu("Move to", move, move.getNumItems() > 0);
        for (auto delta : {-1, 1})
            add(menu, delta < 0 ? "Move Up" : "Move Down",
                [safe, current, delta]
                {
                    if (safe)
                    {
                        CollectionEdit e;
                        e.action = "move";
                        e.id = current.id;
                        e.parent = current.parent;
                        e.position = std::max(0, current.position + delta);
                        safe->submitCollectionEdit(e);
                    }
                });
        menu.addSeparator();
        add(menu, juce::String::fromUTF8("Delete Collection"),
            [safe, node]
            {
                juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::QuestionIcon,
                                                   "Delete Collection?",
                                                   "Remove this collection and its nested collections. "
                                                   "Original audio, Favorites and Designer clips remain.",
                                                   "Delete", "Cancel", safe,
                                                   juce::ModalCallbackFunction::create(
                                                       [safe, node](int result)
                                                       {
                                                           if (result && safe)
                                                           {
                                                               auto e = CollectionEdit{};
                                                               e.action = "delete";
                                                               e.id = node;
                                                               safe->submitCollectionEdit(e);
                                                           }
                                                       }));
            });
    }
    else if (selection)
    {
        auto items = selectedBookmarks();
        add(
            menu, favoriteSelected ? "Remove Favorite" : "Add Favorite",
            [safe, items]
            {
                if (safe)
                {
                    CollectionEdit e;
                    e.action = "favorite";
                    e.items = items;
                    e.enabled = !safe->favoriteSelected;
                    safe->submitCollectionEdit(e);
                }
            },
            !items.empty());
        juce::PopupMenu destinations;
        addCollectionDestinations(destinations, items);
        menu.addSubMenu("Add to Collection", destinations, !items.empty());
        if (collectionScope.isNotEmpty() && collectionScope != "favorites" && collectionScope != "recent")
            add(menu, "Remove from this Collection",
                [safe, items, id = collectionScope]
                {
                    if (safe)
                    {
                        CollectionEdit e;
                        e.action = "member";
                        e.id = id;
                        e.items = items;
                        e.enabled = false;
                        safe->submitCollectionEdit(e);
                    }
                });
    }
    else
    {
        for (auto pair : {std::pair{juce::String(), juce::String("All Sounds")},
                          std::pair{juce::String("favorites"), juce::String("Favorites")},
                          std::pair{juce::String("recent"), juce::String("Recently Used")}})
            add(
                menu, pair.second,
                [safe, id = pair.first]
                {
                    if (safe)
                        safe->selectCollectionScope(id);
                },
                true, collectionScope == pair.first);
        menu.addSeparator();
        std::function<juce::PopupMenu(const juce::String &)> nested = [&](const auto &parent)
        {
            juce::PopupMenu result;
            for (auto &c : snapshot.organization.nodes)
                if (c.parent == parent)
                {
                    bool children =
                        std::any_of(snapshot.organization.nodes.begin(), snapshot.organization.nodes.end(),
                                    [&](auto &n) { return n.parent == c.id; });
                    if (children)
                    {
                        auto sub = nested(c.id);
                        add(sub, "Open " + c.name,
                            [safe, id = c.id]
                            {
                                if (safe)
                                    safe->selectCollectionScope(id);
                            });
                        result.addSubMenu(c.name, sub);
                    }
                    else
                        add(
                            result, c.name,
                            [safe, id = c.id]
                            {
                                if (safe)
                                    safe->selectCollectionScope(id);
                            },
                            true, collectionScope == c.id);
                }
            return result;
        };
        auto collections = nested({});
        menu.addSubMenu("My Collections", collections, !snapshot.organization.nodes.empty());
        juce::PopupMenu roots;
        add(roots, "All sources",
            [safe]
            {
                if (safe)
                {
                    safe->rootFilter.clear();
                    safe->preserveScopeSelection = true;
                    safe->requestSearch();
                    safe->updateScope();
                }
            });
        for (auto &r : snapshot.roots)
            add(roots, juce::File(r.path).getFileName() + (r.online ? "" : " · Offline"),
                [safe, id = r.id]
                {
                    if (safe)
                    {
                        safe->rootFilter = id;
                        safe->preserveScopeSelection = true;
                        safe->requestSearch();
                        safe->updateScope();
                    }
                });
        menu.addSubMenu("Sources", roots);
        if (collectionScope.isNotEmpty() && collectionScope != "favorites" && collectionScope != "recent")
            add(menu, juce::String::fromUTF8("Collection Actions"),
                [safe, id = collectionScope]
                {
                    if (safe)
                        safe->collectionMenu(false, id);
                });
    }
    if (selection && collectionScope.isNotEmpty() && collectionScope != "favorites" &&
        collectionScope != "recent")
        add(menu, juce::String::fromUTF8("Collection Actions"),
            [safe, id = collectionScope]
            {
                if (safe)
                    safe->collectionMenu(false, id);
            });
    menu.addSeparator();
    add(menu, juce::String::fromUTF8("New Collection"),
        [safe]
        {
            if (safe)
                safe->nameCollection();
        });
    add(
        menu, "Undo Organization",
        [safe]
        {
            if (safe)
                safe->submitCollectionEdit(CollectionEdit{"undo"});
        },
        snapshot.organization.canUndo);
    add(
        menu, "Redo Organization",
        [safe]
        {
            if (safe)
                safe->submitCollectionEdit(CollectionEdit{"redo"});
        },
        snapshot.organization.canRedo);
    auto options = juce::PopupMenu::Options().withParentComponent(this);
    if (menuAnchor.isEmpty())
        options = options.withTargetComponent(collectionSelector.isShowing() && node.isEmpty() && !selection
                                                  ? &collectionSelector : &collectionActions);
    else
        options = options.withTargetScreenArea(menuAnchor);
    menu.showMenuAsync(options,
                       [safe, actions = std::move(actions)](int id)
                       {
                           if (safe)
                               if (auto found = actions.find(id); found != actions.end())
                                   found->second();
                       });
}
