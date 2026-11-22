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
    if (preferences)
        preferences->setValue("library.collectionScope", id);
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
    library.editCollections(std::move(edit));
    juce::Component::SafePointer<LibraryView> safe(this);
    juce::MessageManager::callAsync(
        [safe]
        {
            if (safe && safe->isShowing() && !safe->hasKeyboardFocus(true))
                safe->collectionActions.grabKeyboardFocus();
        });
}
void LibraryView::nameCollection(bool rename, const juce::String &node)
{
    juce::String initial = rename ? juce::String() : "New Collection";
    for (auto &c : snapshot.organization.nodes)
        if (c.id == node && rename)
            initial = c.name;
    auto *dialog = new juce::AlertWindow(rename ? "Rename Collection" : "New Collection",
                                         "Virtual collections leave source audio in place.",
                                         juce::MessageBoxIconType::NoIcon);
    dialog->addTextEditor("name", initial, "Name");
    dialog->addButton(rename ? "Rename" : "Create", 1, juce::KeyPress(juce::KeyPress::returnKey));
    dialog->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<LibraryView> safe(this);
    dialog->enterModalState(true,
                            juce::ModalCallbackFunction::create(
                                [safe, dialog, rename, node](int result)
                                {
                                    if (result == 1 && safe)
                                    {
                                        CollectionEdit e;
                                        e.action = rename ? "rename" : "create";
                                        e.id = rename ? node : juce::String();
                                        e.parent = rename ? juce::String() : node;
                                        e.name = dialog->getTextEditorContents("name");
                                        e.position = 1024;
                                        safe->submitCollectionEdit(e);
                                    }
                                }),
                            true);
}
void LibraryView::collectionMenu(bool selection, const juce::String &node)
{
    juce::PopupMenu menu;
    std::map<int, std::function<void()>> actions;
    int nextId = 1;
    juce::Component::SafePointer<LibraryView> safe(this);
    auto collectionPath = [&](juce::String id)
    {
        juce::StringArray names;
        for (int depth = 0; id.isNotEmpty() && depth < 16; ++depth)
        {
            auto it = std::find_if(snapshot.organization.nodes.begin(), snapshot.organization.nodes.end(),
                                   [&](const auto &n) { return n.id == id; });
            if (it == snapshot.organization.nodes.end())
                break;
            names.insert(0, it->name);
            id = it->parent;
        }
        return names.joinIntoString(" / ");
    };
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
        add(menu, "New Subcollection…",
            [safe, node]
            {
                if (safe)
                    safe->nameCollection(false, node);
            });
        add(menu, "Rename…",
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
        destination({}, "My Collections");
        for (auto &c : snapshot.organization.nodes)
            if (c.id != node)
                destination(c.id, collectionPath(c.id));
        menu.addSubMenu("Move to", move);
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
        add(menu, "Delete Collection…",
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
        for (auto &c : snapshot.organization.nodes)
            add(destinations, collectionPath(c.id),
                [safe, items, id = c.id]
                {
                    if (safe)
                    {
                        CollectionEdit e;
                        e.action = "member";
                        e.id = id;
                        e.items = items;
                        safe->submitCollectionEdit(e);
                    }
                });
        menu.addSubMenu("Add to Collection", destinations,
                        !items.empty() && !snapshot.organization.nodes.empty());
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
            add(menu, "Collection Actions…",
                [safe, id = collectionScope]
                {
                    if (safe)
                        safe->collectionMenu(false, id);
                });
    }
    if (selection && collectionScope.isNotEmpty() && collectionScope != "favorites" &&
        collectionScope != "recent")
        add(menu, "Collection Actions…",
            [safe, id = collectionScope]
            {
                if (safe)
                    safe->collectionMenu(false, id);
            });
    menu.addSeparator();
    add(menu, "New Collection…",
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
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(
                           collectionSelector.isShowing() && node.isEmpty() && !selection
                               ? static_cast<juce::Component *>(&collectionSelector)
                               : &collectionActions),
                       [safe, actions = std::move(actions)](int id)
                       {
                           if (safe)
                               if (auto found = actions.find(id); found != actions.end())
                                   found->second();
                       });
}
