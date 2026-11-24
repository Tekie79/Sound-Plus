void LibraryView::metadataPanel(bool filter)
{
    using namespace juce;
    struct Panel : Component
    {
        ComboBox keys, saved;
        TextEditor value, alias;
        Label summary;
        TextButton apply{"Apply"}, unlock{"Use source"}, save{"Save filter"}, suggest{"Apply suggestion"},
            reject{"Dismiss"}, undo{"Undo"}, redo{"Redo"}, exchange{"Manifest"}, aliasSave{"Set alias"};
        Panel()
        {
            for (auto *c : std::initializer_list<Component *>{&keys, &saved, &value, &summary, &apply,
                                                              &unlock, &save, &suggest, &reject, &undo, &redo,
                                                              &exchange, &alias, &aliasSave})
                addAndMakeVisible(c);
        }
    };
    auto p = std::make_unique<Panel>();
    auto *v = p.get();
    auto items = selectedBookmarks();
    if (!filter && items.empty())
        return;
    Component::SafePointer<LibraryView> safe(this);
    auto fields = editableMetadataKeys();
    if (filter)
        fields.addArray(
            {"meta", "channels", "min", "max", "type", "favorite", "generated", "missing", "review"});
    v->keys.setComponentID("metadata-field");
    v->value.setComponentID("metadata-value");
    v->apply.setComponentID("metadata-apply");
    v->keys.addItemList(fields, 1);
    v->keys.setSelectedId(1, dontSendNotification);
    v->summary.setText(filter ? "Filter this Library scope"
                              : String((int)items.size()) + " selected recordings",
                       dontSendNotification);
    v->keys.setBounds(10, 36, 135, 28);
    v->value.setBounds(150, 36, 224, 28);
    v->summary.setBounds(10, 4, 362, 28);
    v->apply.setBounds(10, 70, 75, 28);
    v->value.setTextToShowWhenEmpty("Value", ui::muted);
    auto current = selectedAsset ? cachedRecordingHeader(*selectedAsset) : nullptr;
    var info = current ? current->catalogMetadata : var();
    auto applyQuery = [safe](const String &query)
    {
        if (safe)
        {
            safe->search.setText(query, false);
            safe->requestSearch();
        }
    };
    if (filter)
    {
        auto aliasControls = [v]
        {
            auto key = v->keys.getText();
            bool show = key == "category" || key == "subcategory" || key == "catid";
            v->alias.setVisible(show);
            v->aliasSave.setVisible(show);
            v->setSize(386, show ? 140 : 109);
        };
        v->alias.setBounds(10, 104, 240, 26);
        v->alias.setTextToShowWhenEmpty("Search alias", ui::muted);
        v->aliasSave.setBounds(254, 104, 114, 26);
        v->keys.onChange = aliasControls;
        aliasControls();
        v->aliasSave.onClick = [safe, v]
        {
            if (!safe || v->alias.getText().trim().isEmpty())
                return;
            CollectionEdit e;
            e.action = "meta-alias";
            e.id = v->keys.getText();
            e.name = v->value.getText().trim();
            e.parent = v->alias.getText().trim();
            safe->submitCollectionEdit(std::move(e));
        };
        v->save.setBounds(93, 70, 95, 28);
        v->saved.setBounds(195, 70, 180, 28);
        v->saved.setTextWhenNothingSelected("Saved filters");
        auto filters = std::make_shared<var>(
            preferences ? JSON::parse(preferences->getValue("library.smartFilters")) : var());
        if (!filters->isArray())
            *filters = Array<var>{};
        for (auto &r : *filters->getArray())
            v->saved.addItem(r["query"].toString(), v->saved.getNumItems() + 1);
        auto query = [safe, v]
        {
            auto value = v->value.getText().trim().replaceCharacter('"', ' ').trim();
            auto key = v->keys.getText();
            auto existing = safe ? safe->search.getText().trim() : String{};
            if (value.isEmpty())
                return existing;
            auto clause = key + ":\"" + value + "\"";
            return StringArray::fromTokens(existing, " \t", "\"").contains(clause)
                       ? existing
                       : (existing + " " + clause).trim();
        };
        v->apply.onClick = [applyQuery, query] { applyQuery(query()); };
        v->save.onClick = [safe, v, filters, query]
        {
            if (!safe || !safe->preferences || filters->size() >= 64)
                return;
            auto text = query();
            if (text.isEmpty())
                return;
            auto *o = new DynamicObject;
            var row(o);
            o->setProperty("id", Uuid().toString());
            o->setProperty("query", text);
            o->setProperty("root", safe->rootFilter);
            o->setProperty("scope", safe->collectionScope);
            filters->append(row);
            safe->preferences->setValue("library.smartFilters", JSON::toString(*filters, true));
            v->saved.addItem(text, v->saved.getNumItems() + 1);
        };
        v->saved.onChange = [v, safe, filters, applyQuery]
        {
            auto i = v->saved.getSelectedItemIndex();
            if (!safe || !isPositiveAndBelow(i, filters->size()))
                return;
            auto row = (*filters)[i];
            safe->rootFilter = row["root"].toString();
            safe->selectCollectionScope(row["scope"].toString());
            applyQuery(row["query"].toString());
        };
    }
    else
    {
        if (items.size() == 1 && !info["fields"].hasProperty("category"))
        {
            auto suggestion = suggestedCategory(items[0].asset.name);
            if (suggestion.isNotEmpty() && suggestion != info["rejected"].toString())
            {
                v->suggest.setBounds(10, 104, 234, 26);
                v->reject.setBounds(248, 104, 114, 26);
                v->suggest.setButtonText("Use suggested " + suggestion);
                auto resolve = [safe, items, suggestion](bool yes)
                {
                    if (safe)
                    {
                        CollectionEdit e;
                        e.items = items;
                        e.id = "category";
                        e.name = suggestion;
                        e.action = yes ? "meta-set" : "meta-reject";
                        safe->submitCollectionEdit(std::move(e));
                    }
                };
                v->suggest.onClick = [resolve] { resolve(true); };
                v->reject.onClick = [resolve] { resolve(false); };
            }
        }
        auto currentValue = [v, info]
        { v->value.setText(info["fields"][Identifier(v->keys.getText())].toString(), false); };
        if (items.size() == 1)
        {
            v->keys.onChange = currentValue;
            currentValue();
        }
        auto edit = [safe, v, items](String op)
        {
            if (!safe)
                return;
            CollectionEdit e;
            e.action = op;
            e.id = v->keys.getText();
            e.name = v->value.getText().trim();
            e.items = items;
            safe->submitCollectionEdit(std::move(e));
        };
        v->apply.onClick = [edit] { edit("meta-set"); };
        v->unlock.setBounds(86, 70, 90, 28);
        v->unlock.onClick = [edit] { edit("meta-unlock"); };
        v->undo.setBounds(179, 70, 48, 28);
        v->undo.onClick = [edit] { edit("meta-undo"); };
        v->redo.setBounds(228, 70, 48, 28);
        v->redo.onClick = [edit] { edit("meta-redo"); };
        v->exchange.setBounds(282, 70, 93, 28);
        v->exchange.onClick = [v, safe, items]
        {
            PopupMenu m;
            m.addItem(1, "Export selected metadata...");
            m.addItem(2, "Import user fields...");
            m.addSeparator();
            m.addItem(3, "Mark revision reviewed");
            m.showMenuAsync(
                PopupMenu::Options().withTargetComponent(v->exchange),
                [safe, items](int choice)
                {
                    if (!safe || !choice)
                        return;
                    if (choice == 3)
                    {
                        CollectionEdit e;
                        e.action = "meta-review";
                        e.id = "category";
                        e.items = items;
                        safe->submitCollectionEdit(std::move(e));
                        return;
                    }
                    safe->chooser = std::make_unique<FileChooser>(
                        choice == 1 ? "Export metadata to a new file" : "Import verified metadata", File{},
                        "*.json", true);
                    safe->chooser->launchAsync(
                        (choice == 1 ? FileBrowserComponent::saveMode : FileBrowserComponent::openMode) |
                            FileBrowserComponent::canSelectFiles,
                        [safe, items, choice](const FileChooser &c)
                        {
                            if (!safe || c.getResult() == File())
                                return;
                            CollectionEdit e;
                            e.action = choice == 1 ? "meta-export" : "meta-import";
                            e.items = items;
                            e.name = c.getResult().getFullPathName();
                            safe->submitCollectionEdit(std::move(e));
                        });
                });
        };
    }
    p->setSize(386, v->suggest.getWidth() ? 139 : 109);
    CallOutBox::launchAsynchronously(std::move(p), (filter ? filterMetadata : editTags).getBounds(), this);
}
