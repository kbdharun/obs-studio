/******************************************************************************
    Copyright (C) 2016 by Hugh Bailey <obs.jim@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include <QSpinBox>
#include <QWidgetAction>
#include <QToolTip>
#include <QMessageBox>
#include <util/dstr.hpp>
#include "window-basic-main.hpp"
#include "display-helpers.hpp"
#include "window-namedialog.hpp"
#include "menu-button.hpp"
#include "slider-ignorewheel.hpp"
#include "qt-wrappers.hpp"

#include "obs-hotkey.h"

using namespace std;

Q_DECLARE_METATYPE(OBSScene);
Q_DECLARE_METATYPE(OBSSource);
Q_DECLARE_METATYPE(QuickTransition);

static inline QString MakeQuickTransitionText(QuickTransition *qt)
{
	QString name;

	if (!qt->fadeToBlack)
		name = QT_UTF8(obs_source_get_name(qt->source));
	else
		name = QTStr("FadeToBlack");

	if (!obs_transition_fixed(qt->source))
		name += QString(" (%1ms)").arg(QString::number(qt->duration));
	return name;
}

void OBSBasic::InitDefaultTransitions()
{
	std::vector<OBSSource> transitions;
	size_t idx = 0;
	const char *id;

	/* automatically add transitions that have no configuration (things
	 * such as cut/fade/etc) */
	while (obs_enum_transition_types(idx++, &id)) {
		if (!obs_is_source_configurable(id)) {
			const char *name = obs_source_get_display_name(id);

			OBSSourceAutoRelease tr =
				obs_source_create_private(id, name, NULL);
			InitTransition(tr);
			transitions.emplace_back(tr);

			if (strcmp(id, "fade_transition") == 0)
				fadeTransition = tr;
			else if (strcmp(id, "cut_transition") == 0)
				cutTransition = tr;
		}
	}

	for (OBSSource &tr : transitions) {
		ui->transitions->addItem(QT_UTF8(obs_source_get_name(tr)),
					 QVariant::fromValue(OBSSource(tr)));
	}
}

void OBSBasic::AddQuickTransitionHotkey(QuickTransition *qt)
{
	DStr hotkeyId;
	QString hotkeyName;

	dstr_printf(hotkeyId, "OBSBasic.QuickTransition.%d", qt->id);
	hotkeyName = QTStr("QuickTransitions.HotkeyName")
			     .arg(MakeQuickTransitionText(qt));

	auto quickTransition = [](void *data, obs_hotkey_id, obs_hotkey_t *,
				  bool pressed) {
		int id = (int)(uintptr_t)data;
		OBSBasic *main =
			reinterpret_cast<OBSBasic *>(App()->GetMainWindow());

		if (pressed)
			QMetaObject::invokeMethod(main,
						  "TriggerQuickTransition",
						  Qt::QueuedConnection,
						  Q_ARG(int, id));
	};

	qt->hotkey = obs_hotkey_register_frontend(hotkeyId->array,
						  QT_TO_UTF8(hotkeyName),
						  quickTransition,
						  (void *)(uintptr_t)qt->id);
}

void QuickTransition::SourceRenamed(void *param, calldata_t *data)
{
	QuickTransition *qt = reinterpret_cast<QuickTransition *>(param);

	QString hotkeyName = QTStr("QuickTransitions.HotkeyName")
				     .arg(MakeQuickTransitionText(qt));

	obs_hotkey_set_description(qt->hotkey, QT_TO_UTF8(hotkeyName));

	UNUSED_PARAMETER(data);
}

void OBSBasic::TriggerQuickTransition(int id)
{
	QuickTransition *qt = GetQuickTransition(id);

	if (qt && previewProgramMode) {
		OBSScene scene = GetCurrentScene();
		obs_source_t *source = obs_scene_get_source(scene);

		if (GetCurrentTransition() != qt->source) {
			OverrideTransition(qt->source);
			overridingTransition = true;
		}

		TransitionToScene(source, false, true, qt->duration,
				  qt->fadeToBlack);
	}
}

void OBSBasic::RemoveQuickTransitionHotkey(QuickTransition *qt)
{
	obs_hotkey_unregister(qt->hotkey);
}

void OBSBasic::InitTransition(obs_source_t *transition)
{
	auto onTransitionStop = [](void *data, calldata_t *) {
		OBSBasic *window = (OBSBasic *)data;
		QMetaObject::invokeMethod(window, "TransitionStopped",
					  Qt::QueuedConnection);
	};

	auto onTransitionFullStop = [](void *data, calldata_t *) {
		OBSBasic *window = (OBSBasic *)data;
		QMetaObject::invokeMethod(window, "TransitionFullyStopped",
					  Qt::QueuedConnection);
	};

	signal_handler_t *handler = obs_source_get_signal_handler(transition);
	signal_handler_connect(handler, "transition_video_stop",
			       onTransitionStop, this);
	signal_handler_connect(handler, "transition_stop", onTransitionFullStop,
			       this);
}

static inline OBSSource GetTransitionComboItem(QComboBox *combo, int idx)
{
	return combo->itemData(idx).value<OBSSource>();
}

void OBSBasic::CreateDefaultQuickTransitions()
{
	/* non-configurable transitions are always available, so add them
	 * to the "default quick transitions" list */
	quickTransitions.emplace_back(cutTransition, 300,
				      quickTransitionIdCounter++);
	quickTransitions.emplace_back(fadeTransition, 300,
				      quickTransitionIdCounter++);
	quickTransitions.emplace_back(fadeTransition, 300,
				      quickTransitionIdCounter++, true);
}

void OBSBasic::LoadQuickTransitions(obs_data_array_t *array)
{
	size_t count = obs_data_array_count(array);

	quickTransitionIdCounter = 1;

	for (size_t i = 0; i < count; i++) {
		OBSDataAutoRelease data = obs_data_array_item(array, i);
		OBSDataArrayAutoRelease hotkeys =
			obs_data_get_array(data, "hotkeys");
		const char *name = obs_data_get_string(data, "name");
		int duration = obs_data_get_int(data, "duration");
		int id = obs_data_get_int(data, "id");
		bool toBlack = obs_data_get_bool(data, "fade_to_black");

		if (id) {
			obs_source_t *source = FindTransition(name);
			if (source) {
				quickTransitions.emplace_back(source, duration,
							      id, toBlack);

				if (quickTransitionIdCounter <= id)
					quickTransitionIdCounter = id + 1;

				int idx = (int)quickTransitions.size() - 1;
				AddQuickTransitionHotkey(
					&quickTransitions[idx]);
				obs_hotkey_load(quickTransitions[idx].hotkey,
						hotkeys);
			}
		}
	}
}

obs_data_array_t *OBSBasic::SaveQuickTransitions()
{
	obs_data_array_t *array = obs_data_array_create();

	for (QuickTransition &qt : quickTransitions) {
		OBSDataAutoRelease data = obs_data_create();
		OBSDataArrayAutoRelease hotkeys = obs_hotkey_save(qt.hotkey);

		obs_data_set_string(data, "name",
				    obs_source_get_name(qt.source));
		obs_data_set_int(data, "duration", qt.duration);
		obs_data_set_array(data, "hotkeys", hotkeys);
		obs_data_set_int(data, "id", qt.id);
		obs_data_set_bool(data, "fade_to_black", qt.fadeToBlack);

		obs_data_array_push_back(array, data);
	}

	return array;
}

obs_source_t *OBSBasic::FindTransition(const char *name)
{
	for (int i = 0; i < ui->transitions->count(); i++) {
		OBSSource tr = ui->transitions->itemData(i).value<OBSSource>();
		if (!tr)
			continue;

		const char *trName = obs_source_get_name(tr);
		if (strcmp(trName, name) == 0)
			return tr;
	}

	return nullptr;
}

void OBSBasic::TransitionToScene(OBSScene scene, bool force)
{
	obs_source_t *source = obs_scene_get_source(scene);
	TransitionToScene(source, force);
}

void OBSBasic::TransitionStopped()
{
	if (swapScenesMode) {
		OBSSource scene = OBSGetStrongRef(swapScene);
		if (scene)
			SetCurrentScene(scene);
	}

	EnableTransitionWidgets(true);
	UpdatePreviewProgramIndicators();

	if (api) {
		api->on_event(OBS_FRONTEND_EVENT_TRANSITION_STOPPED);
		api->on_event(OBS_FRONTEND_EVENT_SCENE_CHANGED);
	}

	swapScene = nullptr;
}

void OBSBasic::OverrideTransition(OBSSource transition)
{
	OBSSourceAutoRelease oldTransition = obs_get_output_source(0);

	if (transition != oldTransition) {
		obs_transition_swap_begin(transition, oldTransition);
		obs_set_output_source(0, transition);
		obs_transition_swap_end(transition, oldTransition);
	}
}

void OBSBasic::TransitionFullyStopped()
{
	if (overridingTransition) {
		OverrideTransition(GetCurrentTransition());
		overridingTransition = false;
	}
}

void OBSBasic::TransitionToScene(OBSSource source, bool force,
				 bool quickTransition, int quickDuration,
				 bool black, bool manual)
{
	obs_scene_t *scene = obs_scene_from_source(source);
	bool usingPreviewProgram = IsPreviewProgramMode();
	if (!scene)
		return;

	OBSWeakSource lastProgramScene;

	if (usingPreviewProgram) {
		if (!tBarActive)
			lastProgramScene = programScene;
		programScene = OBSGetWeakRef(source);

		if (!force && !black) {
			OBSSource lastScene = OBSGetStrongRef(lastProgramScene);

			if (!sceneDuplicationMode && lastScene == source)
				return;

			if (swapScenesMode && lastScene &&
			    lastScene != GetCurrentSceneSource())
				swapScene = lastProgramScene;
		}
	}

	if (usingPreviewProgram && sceneDuplicationMode) {
		scene = obs_scene_duplicate(
			scene, obs_source_get_name(obs_scene_get_source(scene)),
			editPropertiesMode ? OBS_SCENE_DUP_PRIVATE_COPY
					   : OBS_SCENE_DUP_PRIVATE_REFS);
		source = obs_scene_get_source(scene);
	}

	OBSSourceAutoRelease transition = obs_get_output_source(0);
	if (!transition) {
		if (usingPreviewProgram && sceneDuplicationMode)
			obs_scene_release(scene);
		return;
	}

	float t = obs_transition_get_time(transition);
	bool stillTransitioning = t < 1.0f && t > 0.0f;

	// If actively transitioning, block new transitions from starting
	if (usingPreviewProgram && stillTransitioning)
		goto cleanup;

	if (force) {
		obs_transition_set(transition, source);
		if (api)
			api->on_event(OBS_FRONTEND_EVENT_SCENE_CHANGED);
	} else {
		int duration = ui->transitionDuration->value();

		/* check for scene override */
		OBSSource trOverride = GetOverrideTransition(source);

		if (trOverride && !overridingTransition && !quickTransition) {
			transition = std::move(trOverride);
			duration = GetOverrideTransitionDuration(source);
			OverrideTransition(transition.Get());
			overridingTransition = true;
		}

		if (black && !prevFTBSource) {
			prevFTBSource = source;
			source = nullptr;
		} else if (black && prevFTBSource) {
			source = prevFTBSource;
			prevFTBSource = nullptr;
		} else if (!black) {
			prevFTBSource = nullptr;
		}

		if (quickTransition)
			duration = quickDuration;

		enum obs_transition_mode mode =
			manual ? OBS_TRANSITION_MODE_MANUAL
			       : OBS_TRANSITION_MODE_AUTO;

		EnableTransitionWidgets(false);

		bool success = obs_transition_start(transition, mode, duration,
						    source);

		if (!success)
			TransitionFullyStopped();
	}

cleanup:
	if (usingPreviewProgram && sceneDuplicationMode)
		obs_scene_release(scene);
}

static inline void SetComboTransition(QComboBox *combo, obs_source_t *tr)
{
	int idx = combo->findData(QVariant::fromValue<OBSSource>(tr));
	if (idx != -1) {
		combo->blockSignals(true);
		combo->setCurrentIndex(idx);
		combo->blockSignals(false);
	}
}

void OBSBasic::SetTransition(OBSSource transition)
{
	OBSSourceAutoRelease oldTransition = obs_get_output_source(0);

	if (oldTransition && transition) {
		obs_transition_swap_begin(transition, oldTransition);
		if (transition != GetCurrentTransition())
			SetComboTransition(ui->transitions, transition);
		obs_set_output_source(0, transition);
		obs_transition_swap_end(transition, oldTransition);
	} else {
		obs_set_output_source(0, transition);
	}

	bool fixed = transition ? obs_transition_fixed(transition) : false;
	ui->transitionDurationLabel->setVisible(!fixed);
	ui->transitionDuration->setVisible(!fixed);

	bool configurable = obs_source_configurable(transition);
	ui->transitionRemove->setEnabled(configurable);
	ui->transitionProps->setEnabled(configurable);

	if (api)
		api->on_event(OBS_FRONTEND_EVENT_TRANSITION_CHANGED);
}

OBSSource OBSBasic::GetCurrentTransition()
{
	return ui->transitions->currentData().value<OBSSource>();
}

void OBSBasic::on_transitions_currentIndexChanged(int)
{
	OBSSource transition = GetCurrentTransition();
	SetTransition(transition);
}

void OBSBasic::AddTransition()
{
	QAction *action = reinterpret_cast<QAction *>(sender());
	QString idStr = action->property("id").toString();

	string name;
	QString placeHolderText =
		QT_UTF8(obs_source_get_display_name(QT_TO_UTF8(idStr)));
	QString format = placeHolderText + " (%1)";
	obs_source_t *source = nullptr;
	int i = 1;

	while ((source = FindTransition(QT_TO_UTF8(placeHolderText)))) {
		placeHolderText = format.arg(++i);
	}

	bool accepted = NameDialog::AskForName(this,
					       QTStr("TransitionNameDlg.Title"),
					       QTStr("TransitionNameDlg.Text"),
					       name, placeHolderText);

	if (accepted) {
		if (name.empty()) {
			OBSMessageBox::warning(this,
					       QTStr("NoNameEntered.Title"),
					       QTStr("NoNameEntered.Text"));
			AddTransition();
			return;
		}

		source = FindTransition(name.c_str());
		if (source) {
			OBSMessageBox::warning(this, QTStr("NameExists.Title"),
					       QTStr("NameExists.Text"));

			AddTransition();
			return;
		}

		source = obs_source_create_private(QT_TO_UTF8(idStr),
						   name.c_str(), NULL);
		InitTransition(source);
		ui->transitions->addItem(
			QT_UTF8(name.c_str()),
			QVariant::fromValue(OBSSource(source)));
		ui->transitions->setCurrentIndex(ui->transitions->count() - 1);
		CreatePropertiesWindow(source);
		obs_source_release(source);

		if (api)
			api->on_event(
				OBS_FRONTEND_EVENT_TRANSITION_LIST_CHANGED);

		ClearQuickTransitionWidgets();
		RefreshQuickTransitions();
	}
}

void OBSBasic::on_transitionAdd_clicked()
{
	bool foundConfigurableTransitions = false;
	QMenu menu(this);
	size_t idx = 0;
	const char *id;

	while (obs_enum_transition_types(idx++, &id)) {
		if (obs_is_source_configurable(id)) {
			const char *name = obs_source_get_display_name(id);
			QAction *action = new QAction(name, this);
			action->setProperty("id", id);

			connect(action, SIGNAL(triggered()), this,
				SLOT(AddTransition()));

			menu.addAction(action);
			foundConfigurableTransitions = true;
		}
	}

	if (foundConfigurableTransitions)
		menu.exec(QCursor::pos());
}

void OBSBasic::on_transitionRemove_clicked()
{
	OBSSource tr = GetCurrentTransition();

	if (!tr || !obs_source_configurable(tr) || !QueryRemoveSource(tr))
		return;

	int idx = ui->transitions->findData(QVariant::fromValue<OBSSource>(tr));
	if (idx == -1)
		return;

	for (size_t i = quickTransitions.size(); i > 0; i--) {
		QuickTransition &qt = quickTransitions[i - 1];
		if (qt.source == tr) {
			if (qt.button)
				qt.button->deleteLater();
			RemoveQuickTransitionHotkey(&qt);
			quickTransitions.erase(quickTransitions.begin() + i -
					       1);
		}
	}

	ui->transitions->removeItem(idx);

	if (api)
		api->on_event(OBS_FRONTEND_EVENT_TRANSITION_LIST_CHANGED);

	ClearQuickTransitionWidgets();
	RefreshQuickTransitions();
}

void OBSBasic::RenameTransition()
{
	QAction *action = reinterpret_cast<QAction *>(sender());
	QVariant variant = action->property("transition");
	obs_source_t *transition = variant.value<OBSSource>();

	string name;
	QString placeHolderText = QT_UTF8(obs_source_get_name(transition));
	obs_source_t *source = nullptr;

	bool accepted = NameDialog::AskForName(this,
					       QTStr("TransitionNameDlg.Title"),
					       QTStr("TransitionNameDlg.Text"),
					       name, placeHolderText);

	if (!accepted)
		return;
	if (name.empty()) {
		OBSMessageBox::warning(this, QTStr("NoNameEntered.Title"),
				       QTStr("NoNameEntered.Text"));
		RenameTransition();
		return;
	}

	source = FindTransition(name.c_str());
	if (source) {
		OBSMessageBox::warning(this, QTStr("NameExists.Title"),
				       QTStr("NameExists.Text"));

		RenameTransition();
		return;
	}

	obs_source_set_name(transition, name.c_str());
	int idx = ui->transitions->findData(variant);
	if (idx != -1) {
		ui->transitions->setItemText(idx, QT_UTF8(name.c_str()));

		if (api)
			api->on_event(
				OBS_FRONTEND_EVENT_TRANSITION_LIST_CHANGED);

		ClearQuickTransitionWidgets();
		RefreshQuickTransitions();
	}
}

void OBSBasic::on_transitionProps_clicked()
{
	OBSSource source = GetCurrentTransition();

	if (!obs_source_configurable(source))
		return;

	auto properties = [&]() { CreatePropertiesWindow(source); };

	QMenu menu(this);

	QAction *action = new QAction(QTStr("Rename"), &menu);
	connect(action, SIGNAL(triggered()), this, SLOT(RenameTransition()));
	action->setProperty("transition", QVariant::fromValue(source));
	menu.addAction(action);

	action = new QAction(QTStr("Properties"), &menu);
	connect(action, &QAction::triggered, properties);
	menu.addAction(action);

	menu.exec(QCursor::pos());
}

void OBSBasic::on_transitionDuration_valueChanged(int value)
{
	if (api) {
		api->on_event(OBS_FRONTEND_EVENT_TRANSITION_DURATION_CHANGED);
	}

	UNUSED_PARAMETER(value);
}

QuickTransition *OBSBasic::GetQuickTransition(int id)
{
	for (QuickTransition &qt : quickTransitions) {
		if (qt.id == id)
			return &qt;
	}

	return nullptr;
}

int OBSBasic::GetQuickTransitionIdx(int id)
{
	for (int idx = 0; idx < (int)quickTransitions.size(); idx++) {
		QuickTransition &qt = quickTransitions[idx];

		if (qt.id == id)
			return idx;
	}

	return -1;
}

void OBSBasic::SetCurrentScene(obs_scene_t *scene, bool force)
{
	obs_source_t *source = obs_scene_get_source(scene);
	SetCurrentScene(source, force);
}

template<typename T> static T GetOBSRef(QListWidgetItem *item)
{
	return item->data(static_cast<int>(QtDataRole::OBSRef)).value<T>();
}

void OBSBasic::SetCurrentScene(OBSSource scene, bool force)
{
	if (!IsPreviewProgramMode()) {
		TransitionToScene(scene, force);
	} else {
		OBSSource actualLastScene = OBSGetStrongRef(lastScene);
		if (actualLastScene != scene) {
			if (scene)
				obs_source_inc_showing(scene);
			if (actualLastScene)
				obs_source_dec_showing(actualLastScene);
			lastScene = OBSGetWeakRef(scene);
		}
	}

	if (obs_scene_get_source(GetCurrentScene()) != scene) {
		for (int i = 0; i < ui->scenes->count(); i++) {
			QListWidgetItem *item = ui->scenes->item(i);
			OBSScene itemScene = GetOBSRef<OBSScene>(item);
			obs_source_t *source = obs_scene_get_source(itemScene);

			if (source == scene) {
				ui->scenes->blockSignals(true);
				currentScene = itemScene.Get();
				ui->scenes->setCurrentItem(item);
				ui->scenes->blockSignals(false);
				if (api)
					api->on_event(
						OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED);
				break;
			}
		}
	}

	UpdateContextBar(true);
	UpdatePreviewProgramIndicators();

	if (scene) {
		bool userSwitched = (!force && !disableSaving);
		blog(LOG_INFO, "%s to scene '%s'",
		     userSwitched ? "User switched" : "Switched",
		     obs_source_get_name(scene));
	}
}

void OBSBasic::CreateProgramDisplay()
{
	program = new OBSQTDisplay();

	program->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(program.data(), &QWidget::customContextMenuRequested, this,
		&OBSBasic::ProgramViewContextMenuRequested);

	auto displayResize = [this]() {
		struct obs_video_info ovi;

		if (obs_get_video_info(&ovi))
			ResizeProgram(ovi.base_width, ovi.base_height);
	};

	connect(program.data(), &OBSQTDisplay::DisplayResized, displayResize);

	auto addDisplay = [this](OBSQTDisplay *window) {
		obs_display_add_draw_callback(window->GetDisplay(),
					      OBSBasic::RenderProgram, this);

		struct obs_video_info ovi;
		if (obs_get_video_info(&ovi))
			ResizeProgram(ovi.base_width, ovi.base_height);
	};

	connect(program.data(), &OBSQTDisplay::DisplayCreated, addDisplay);

	program->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void OBSBasic::TransitionClicked()
{
	if (previewProgramMode)
		TransitionToScene(GetCurrentScene());
}

#define T_BAR_PRECISION 1024
#define T_BAR_PRECISION_F ((float)T_BAR_PRECISION)
#define T_BAR_CLAMP (T_BAR_PRECISION / 10)

void OBSBasic::CreateProgramOptions()
{
	programOptions = new QWidget();
	QVBoxLayout *layout = new QVBoxLayout();
	layout->setSpacing(4);

	QPushButton *configTransitions = new QPushButton();
	configTransitions->setProperty("themeID", "menuIconSmall");

	QHBoxLayout *mainButtonLayout = new QHBoxLayout();
	mainButtonLayout->setSpacing(2);

	transitionButton = new QPushButton(QTStr("Transition"));
	transitionButton->setSizePolicy(QSizePolicy::Expanding,
					QSizePolicy::Preferred);

	QHBoxLayout *quickTransitions = new QHBoxLayout();
	quickTransitions->setSpacing(2);

	QPushButton *addQuickTransition = new QPushButton();
	addQuickTransition->setProperty("themeID", "addIconSmall");

	QLabel *quickTransitionsLabel = new QLabel(QTStr("QuickTransitions"));
	quickTransitionsLabel->setSizePolicy(QSizePolicy::Expanding,
					     QSizePolicy::Preferred);

	quickTransitions->addWidget(quickTransitionsLabel);
	quickTransitions->addWidget(addQuickTransition);

	mainButtonLayout->addWidget(transitionButton);
	mainButtonLayout->addWidget(configTransitions);

	tBar = new SliderIgnoreScroll(Qt::Horizontal);
	tBar->setMinimum(0);
	tBar->setMaximum(T_BAR_PRECISION - 1);

	tBar->setProperty("themeID", "tBarSlider");

	connect(tBar, SIGNAL(sliderMoved(int)), this, SLOT(TBarChanged(int)));
	connect(tBar, SIGNAL(valueChanged(int)), this,
		SLOT(on_tbar_position_valueChanged(int)));
	connect(tBar, SIGNAL(sliderReleased()), this, SLOT(TBarReleased()));

	layout->addStretch(0);
	layout->addLayout(mainButtonLayout);
	layout->addLayout(quickTransitions);
	layout->addWidget(tBar);
	layout->addStretch(0);

	programOptions->setLayout(layout);

	auto onAdd = [this]() {
		QScopedPointer<QMenu> menu(CreateTransitionMenu(this, nullptr));
		menu->exec(QCursor::pos());
	};

	auto onConfig = [this]() {
		QMenu menu(this);
		QAction *action;

		auto toggleEditProperties = [this]() {
			editPropertiesMode = !editPropertiesMode;

			OBSSource actualScene = OBSGetStrongRef(programScene);
			if (actualScene)
				TransitionToScene(actualScene, true);
		};

		auto toggleSwapScenesMode = [this]() {
			swapScenesMode = !swapScenesMode;
		};

		auto toggleSceneDuplication = [this]() {
			sceneDuplicationMode = !sceneDuplicationMode;

			OBSSource actualScene = OBSGetStrongRef(programScene);
			if (actualScene)
				TransitionToScene(actualScene, true);
		};

		auto showToolTip = [&]() {
			QAction *act = menu.activeAction();
			QToolTip::showText(QCursor::pos(), act->toolTip(),
					   &menu, menu.actionGeometry(act));
		};

		action = menu.addAction(
			QTStr("QuickTransitions.DuplicateScene"));
		action->setToolTip(QTStr("QuickTransitions.DuplicateSceneTT"));
		action->setCheckable(true);
		action->setChecked(sceneDuplicationMode);
		connect(action, &QAction::triggered, toggleSceneDuplication);
		connect(action, &QAction::hovered, showToolTip);

		action = menu.addAction(
			QTStr("QuickTransitions.EditProperties"));
		action->setToolTip(QTStr("QuickTransitions.EditPropertiesTT"));
		action->setCheckable(true);
		action->setChecked(editPropertiesMode);
		action->setEnabled(sceneDuplicationMode);
		connect(action, &QAction::triggered, toggleEditProperties);
		connect(action, &QAction::hovered, showToolTip);

		action = menu.addAction(QTStr("QuickTransitions.SwapScenes"));
		action->setToolTip(QTStr("QuickTransitions.SwapScenesTT"));
		action->setCheckable(true);
		action->setChecked(swapScenesMode);
		connect(action, &QAction::triggered, toggleSwapScenesMode);
		connect(action, &QAction::hovered, showToolTip);

		menu.exec(QCursor::pos());
	};

	connect(transitionButton.data(), &QAbstractButton::clicked, this,
		&OBSBasic::TransitionClicked);
	connect(addQuickTransition, &QAbstractButton::clicked, onAdd);
	connect(configTransitions, &QAbstractButton::clicked, onConfig);
}

void OBSBasic::TBarReleased()
{
	int val = tBar->value();

	OBSSourceAutoRelease transition = obs_get_output_source(0);

	if ((tBar->maximum() - val) <= T_BAR_CLAMP) {
		obs_transition_set_manual_time(transition, 1.0f);
		tBar->blockSignals(true);
		tBar->setValue(0);
		tBar->blockSignals(false);
		tBarActive = false;
		EnableTransitionWidgets(true);

	} else if (val <= T_BAR_CLAMP) {
		obs_transition_set_manual_time(transition, 0.0f);
		TransitionFullyStopped();
		tBar->blockSignals(true);
		tBar->setValue(0);
		tBar->blockSignals(false);
		tBarActive = false;
		EnableTransitionWidgets(true);
	}

	tBar->clearFocus();
}

static bool ValidTBarTransition(OBSSource transition)
{
	if (!transition)
		return false;

	QString id = QT_UTF8(obs_source_get_id(transition));

	if (id == "cut_transition" || id == "obs_stinger_transition")
		return false;

	return true;
}

void OBSBasic::TBarChanged(int value)
{
	OBSSourceAutoRelease transition = obs_get_output_source(0);

	tBar->setValue(value);

	if (!tBarActive) {
		OBSSource sceneSource = GetCurrentSceneSource();
		OBSSource tBarTr = GetOverrideTransition(sceneSource);

		if (!ValidTBarTransition(tBarTr)) {
			tBarTr = GetCurrentTransition();

			if (!ValidTBarTransition(tBarTr))
				tBarTr = FindTransition(
					obs_source_get_display_name(
						"fade_transition"));

			OverrideTransition(tBarTr);
			overridingTransition = true;

			transition = std::move(tBarTr);
		}

		obs_transition_set_manual_torque(transition, 8.0f, 0.05f);
		TransitionToScene(sceneSource, false, false, false, 0, true);
		tBarActive = true;
	}

	obs_transition_set_manual_time(transition,
				       (float)value / T_BAR_PRECISION_F);
}

int OBSBasic::GetTbarPosition()
{
	return tBar->value();
}

void OBSBasic::on_tbar_position_valueChanged(int value)
{
	if (api) {
		api->on_event(OBS_FRONTEND_EVENT_TBAR_VALUE_CHANGED);
	}

	UNUSED_PARAMETER(value);
}
void OBSBasic::on_modeSwitch_clicked()
{
	SetPreviewProgramMode(!IsPreviewProgramMode());
}

static inline void ResetQuickTransitionText(QuickTransition *qt)
{
	qt->button->setText(MakeQuickTransitionText(qt));
}

QMenu *OBSBasic::CreatePerSceneTransitionMenu()
{
	OBSSource scene = GetCurrentSceneSource();
	QMenu *menu = new QMenu(QTStr("TransitionOverride"));
	QAction *action;

	OBSDataAutoRelease data = obs_source_get_private_settings(scene);

	obs_data_set_default_int(data, "transition_duration", 300);

	const char *curTransition = obs_data_get_string(data, "transition");
	int curDuration = (int)obs_data_get_int(data, "transition_duration");

	QSpinBox *duration = new QSpinBox(menu);
	duration->setMinimum(50);
	duration->setSuffix(" ms");
	duration->setMaximum(20000);
	duration->setSingleStep(50);
	duration->setValue(curDuration);

	auto setTransition = [this](QAction *action) {
		int idx = action->property("transition_index").toInt();
		OBSSource scene = GetCurrentSceneSource();
		OBSDataAutoRelease data =
			obs_source_get_private_settings(scene);

		if (idx == -1) {
			obs_data_set_string(data, "transition", "");
			return;
		}

		OBSSource tr = GetTransitionComboItem(ui->transitions, idx);

		if (tr) {
			const char *name = obs_source_get_name(tr);
			obs_data_set_string(data, "transition", name);
		}
	};

	auto setDuration = [this](int duration) {
		OBSSource scene = GetCurrentSceneSource();
		OBSDataAutoRelease data =
			obs_source_get_private_settings(scene);

		obs_data_set_int(data, "transition_duration", duration);
	};

	connect(duration, (void(QSpinBox::*)(int)) & QSpinBox::valueChanged,
		setDuration);

	for (int i = -1; i < ui->transitions->count(); i++) {
		const char *name = "";

		if (i >= 0) {
			OBSSource tr;
			tr = GetTransitionComboItem(ui->transitions, i);
			if (!tr)
				continue;
			name = obs_source_get_name(tr);
		}

		bool match = (name && strcmp(name, curTransition) == 0);

		if (!name || !*name)
			name = Str("None");

		action = menu->addAction(QT_UTF8(name));
		action->setProperty("transition_index", i);
		action->setCheckable(true);
		action->setChecked(match);

		connect(action, &QAction::triggered,
			std::bind(setTransition, action));
	}

	QWidgetAction *durationAction = new QWidgetAction(menu);
	durationAction->setDefaultWidget(duration);

	menu->addSeparator();
	menu->addAction(durationAction);
	return menu;
}

void OBSBasic::on_actionShowTransitionProperties_triggered()
{
	OBSSceneItem item = GetCurrentSceneItem();
	OBSSource source = obs_sceneitem_get_transition(item, true);

	if (source)
		CreatePropertiesWindow(source);
}

void OBSBasic::on_actionHideTransitionProperties_triggered()
{
	OBSSceneItem item = GetCurrentSceneItem();
	OBSSource source = obs_sceneitem_get_transition(item, false);

	if (source)
		CreatePropertiesWindow(source);
}

void OBSBasic::PasteShowHideTransition(obs_sceneitem_t *item, bool show,
				       obs_source_t *tr)
{
	int64_t sceneItemId = obs_sceneitem_get_id(item);
	std::string sceneName = obs_source_get_name(
		obs_scene_get_source(obs_sceneitem_get_scene(item)));

	auto undo_redo = [sceneName, sceneItemId,
			  show](const std::string &data) {
		OBSSourceAutoRelease source =
			obs_get_source_by_name(sceneName.c_str());
		obs_scene_t *scene = obs_scene_from_source(source);
		obs_sceneitem_t *i =
			obs_scene_find_sceneitem_by_id(scene, sceneItemId);
		if (i) {
			OBSDataAutoRelease dat =
				obs_data_create_from_json(data.c_str());
			obs_sceneitem_transition_load(i, dat, show);
		}
	};

	OBSDataAutoRelease oldTransitionData =
		obs_sceneitem_transition_save(item, show);

	OBSSourceAutoRelease dup =
		obs_source_duplicate(tr, obs_source_get_name(tr), true);
	obs_sceneitem_set_transition(item, show, dup);

	OBSDataAutoRelease transitionData =
		obs_sceneitem_transition_save(item, show);

	std::string undo_data(obs_data_get_json(oldTransitionData));
	std::string redo_data(obs_data_get_json(transitionData));
	if (undo_data.compare(redo_data) == 0)
		return;

	QString text = show ? QTStr("Undo.ShowTransition")
			    : QTStr("Undo.HideTransition");
	const char *name = obs_source_get_name(obs_sceneitem_get_source(item));
	undo_s.add_action(text.arg(name), undo_redo, undo_redo, undo_data,
			  redo_data);
}

QMenu *OBSBasic::CreateVisibilityTransitionMenu(bool visible)
{
	OBSSceneItem si = GetCurrentSceneItem();

	QMenu *menu =
		new QMenu(QTStr(visible ? "ShowTransition" : "HideTransition"));
	QAction *action;

	OBSSource curTransition = obs_sceneitem_get_transition(si, visible);
	const char *curId = curTransition ? obs_source_get_id(curTransition)
					  : nullptr;
	int curDuration =
		(int)obs_sceneitem_get_transition_duration(si, visible);

	if (curDuration <= 0)
		curDuration = obs_frontend_get_transition_duration();

	QSpinBox *duration = new QSpinBox(menu);
	duration->setMinimum(50);
	duration->setSuffix(" ms");
	duration->setMaximum(20000);
	duration->setSingleStep(50);
	duration->setValue(curDuration);

	auto setTransition = [this](QAction *action, bool visible) {
		OBSBasic *main =
			reinterpret_cast<OBSBasic *>(App()->GetMainWindow());

		QString id = action->property("transition_id").toString();
		OBSSceneItem sceneItem = main->GetCurrentSceneItem();
		int64_t sceneItemId = obs_sceneitem_get_id(sceneItem);
		std::string sceneName =
			obs_source_get_name(obs_scene_get_source(
				obs_sceneitem_get_scene(sceneItem)));

		auto undo_redo = [sceneName, sceneItemId,
				  visible](const std::string &data) {
			OBSSourceAutoRelease source =
				obs_get_source_by_name(sceneName.c_str());
			obs_scene_t *scene = obs_scene_from_source(source);
			obs_sceneitem_t *i = obs_scene_find_sceneitem_by_id(
				scene, sceneItemId);
			if (i) {
				OBSDataAutoRelease dat =
					obs_data_create_from_json(data.c_str());
				obs_sceneitem_transition_load(i, dat, visible);
			}
		};
		OBSDataAutoRelease oldTransitionData =
			obs_sceneitem_transition_save(sceneItem, visible);
		if (id.isNull() || id.isEmpty()) {
			obs_sceneitem_set_transition(sceneItem, visible,
						     nullptr);
		} else {
			OBSSource tr = obs_sceneitem_get_transition(sceneItem,
								    visible);

			if (!tr || strcmp(QT_TO_UTF8(id),
					  obs_source_get_id(tr)) != 0) {
				QString name = QT_UTF8(obs_source_get_name(
					obs_sceneitem_get_source(sceneItem)));
				name += " ";
				name += QTStr(visible ? "ShowTransition"
						      : "HideTransition");
				tr = obs_source_create_private(QT_TO_UTF8(id),
							       QT_TO_UTF8(name),
							       nullptr);
				obs_sceneitem_set_transition(sceneItem, visible,
							     tr);
				obs_source_release(tr);

				int duration = (int)
					obs_sceneitem_get_transition_duration(
						sceneItem, visible);
				if (duration <= 0) {
					duration =
						obs_frontend_get_transition_duration();
					obs_sceneitem_set_transition_duration(
						sceneItem, visible, duration);
				}
			}
			if (obs_source_configurable(tr))
				CreatePropertiesWindow(tr);
		}
		OBSDataAutoRelease newTransitionData =
			obs_sceneitem_transition_save(sceneItem, visible);
		std::string undo_data(obs_data_get_json(oldTransitionData));
		std::string redo_data(obs_data_get_json(newTransitionData));
		if (undo_data.compare(redo_data) != 0)
			main->undo_s.add_action(
				QTStr(visible ? "Undo.ShowTransition"
					      : "Undo.HideTransition")
					.arg(obs_source_get_name(
						obs_sceneitem_get_source(
							sceneItem))),
				undo_redo, undo_redo, undo_data, redo_data);
	};
	auto setDuration = [visible](int duration) {
		OBSBasic *main =
			reinterpret_cast<OBSBasic *>(App()->GetMainWindow());

		OBSSceneItem item = main->GetCurrentSceneItem();
		obs_sceneitem_set_transition_duration(item, visible, duration);
	};
	connect(duration, (void(QSpinBox::*)(int)) & QSpinBox::valueChanged,
		setDuration);

	action = menu->addAction(QT_UTF8(Str("None")));
	action->setProperty("transition_id", QT_UTF8(""));
	action->setCheckable(true);
	action->setChecked(!curId);
	connect(action, &QAction::triggered,
		std::bind(setTransition, action, visible));
	size_t idx = 0;
	const char *id;
	while (obs_enum_transition_types(idx++, &id)) {
		const char *name = obs_source_get_display_name(id);
		const bool match = id && curId && strcmp(id, curId) == 0;
		action = menu->addAction(QT_UTF8(name));
		action->setProperty("transition_id", QT_UTF8(id));
		action->setCheckable(true);
		action->setChecked(match);
		connect(action, &QAction::triggered,
			std::bind(setTransition, action, visible));
	}

	QWidgetAction *durationAction = new QWidgetAction(menu);
	durationAction->setDefaultWidget(duration);

	menu->addSeparator();
	menu->addAction(durationAction);
	if (curId && obs_is_source_configurable(curId)) {
		menu->addSeparator();
		menu->addAction(
			QTStr("Properties"), this,
			visible ? SLOT(on_actionShowTransitionProperties_triggered())
				: SLOT(on_actionHideTransitionProperties_triggered()));
	}

	auto copyTransition = [this](QAction *, bool visible) {
		OBSBasic *main =
			reinterpret_cast<OBSBasic *>(App()->GetMainWindow());
		OBSSceneItem item = main->GetCurrentSceneItem();
		obs_source_t *tr = obs_sceneitem_get_transition(item, visible);
		main->copySourceTransition = obs_source_get_weak_source(tr);
	};
	menu->addSeparator();
	action = menu->addAction(QT_UTF8(Str("Copy")));
	action->setEnabled(curId != nullptr);
	connect(action, &QAction::triggered,
		std::bind(copyTransition, action, visible));

	auto pasteTransition = [this](QAction *, bool show) {
		OBSBasic *main =
			reinterpret_cast<OBSBasic *>(App()->GetMainWindow());
		OBSSource tr = OBSGetStrongRef(main->copySourceTransition);
		if (!tr)
			return;

		for (auto &selectedSource : GetAllSelectedSourceItems()) {
			OBSSceneItem item =
				main->ui->sources->Get(selectedSource.row());
			if (!item)
				continue;

			PasteShowHideTransition(item, show, tr);
		}
	};

	action = menu->addAction(QT_UTF8(Str("Paste")));
	action->setEnabled(!!OBSGetStrongRef(copySourceTransition));
	connect(action, &QAction::triggered,
		std::bind(pasteTransition, action, visible));
	return menu;
}

QMenu *OBSBasic::CreateTransitionMenu(QWidget *parent, QuickTransition *qt)
{
	QMenu *menu = new QMenu(parent);
	QAction *action;
	OBSSource tr;

	if (qt) {
		action = menu->addAction(QTStr("Remove"));
		action->setProperty("id", qt->id);
		connect(action, &QAction::triggered, this,
			&OBSBasic::QuickTransitionRemoveClicked);

		menu->addSeparator();
	}

	QSpinBox *duration = new QSpinBox(menu);
	if (qt)
		duration->setProperty("id", qt->id);
	duration->setMinimum(50);
	duration->setSuffix(" ms");
	duration->setMaximum(20000);
	duration->setSingleStep(50);
	duration->setValue(qt ? qt->duration : 300);

	if (qt) {
		connect(duration,
			(void(QSpinBox::*)(int)) & QSpinBox::valueChanged, this,
			&OBSBasic::QuickTransitionChangeDuration);
	}

	tr = fadeTransition;

	action = menu->addAction(QTStr("FadeToBlack"));
	action->setProperty("fadeToBlack", true);

	if (qt) {
		action->setProperty("id", qt->id);
		connect(action, &QAction::triggered, this,
			&OBSBasic::QuickTransitionChange);
	} else {
		action->setProperty("duration",
				    QVariant::fromValue<QWidget *>(duration));
		connect(action, &QAction::triggered, this,
			&OBSBasic::AddQuickTransition);
	}

	for (int i = 0; i < ui->transitions->count(); i++) {
		tr = GetTransitionComboItem(ui->transitions, i);

		if (!tr)
			continue;

		action = menu->addAction(obs_source_get_name(tr));
		action->setProperty("transition_index", i);

		if (qt) {
			action->setProperty("id", qt->id);
			connect(action, &QAction::triggered, this,
				&OBSBasic::QuickTransitionChange);
		} else {
			action->setProperty(
				"duration",
				QVariant::fromValue<QWidget *>(duration));
			connect(action, &QAction::triggered, this,
				&OBSBasic::AddQuickTransition);
		}
	}

	QWidgetAction *durationAction = new QWidgetAction(menu);
	durationAction->setDefaultWidget(duration);

	menu->addSeparator();
	menu->addAction(durationAction);
	return menu;
}

void OBSBasic::AddQuickTransitionId(int id)
{
	QuickTransition *qt = GetQuickTransition(id);
	if (!qt)
		return;

	/* --------------------------------- */

	QPushButton *button = new MenuButton();
	button->setProperty("id", id);

	qt->button = button;
	ResetQuickTransitionText(qt);

	/* --------------------------------- */

	QMenu *buttonMenu = CreateTransitionMenu(button, qt);

	/* --------------------------------- */

	button->setMenu(buttonMenu);
	connect(button, &QAbstractButton::clicked, this,
		&OBSBasic::QuickTransitionClicked);

	QVBoxLayout *programLayout =
		reinterpret_cast<QVBoxLayout *>(programOptions->layout());

	int idx = 3;
	for (;; idx++) {
		QLayoutItem *item = programLayout->itemAt(idx);
		if (!item)
			break;

		QWidget *widget = item->widget();
		if (!widget || !widget->property("id").isValid())
			break;
	}

	programLayout->insertWidget(idx, button);
}

void OBSBasic::AddQuickTransition()
{
	int trIdx = sender()->property("transition_index").toInt();
	QSpinBox *duration = sender()->property("duration").value<QSpinBox *>();
	bool fadeToBlack = sender()->property("fadeToBlack").value<bool>();
	OBSSource transition =
		fadeToBlack ? OBSSource(fadeTransition)
			    : GetTransitionComboItem(ui->transitions, trIdx);

	if (!transition)
		return;

	int id = quickTransitionIdCounter++;

	quickTransitions.emplace_back(transition, duration->value(), id,
				      fadeToBlack);
	AddQuickTransitionId(id);

	int idx = (int)quickTransitions.size() - 1;
	AddQuickTransitionHotkey(&quickTransitions[idx]);
}

void OBSBasic::ClearQuickTransitions()
{
	for (QuickTransition &qt : quickTransitions)
		RemoveQuickTransitionHotkey(&qt);
	quickTransitions.clear();

	if (!programOptions)
		return;

	QVBoxLayout *programLayout =
		reinterpret_cast<QVBoxLayout *>(programOptions->layout());

	for (int idx = 0;; idx++) {
		QLayoutItem *item = programLayout->itemAt(idx);
		if (!item)
			break;

		QWidget *widget = item->widget();
		if (!widget)
			continue;

		int id = widget->property("id").toInt();
		if (id != 0) {
			delete widget;
			idx--;
		}
	}
}

void OBSBasic::QuickTransitionClicked()
{
	int id = sender()->property("id").toInt();
	TriggerQuickTransition(id);
}

void OBSBasic::QuickTransitionChange()
{
	int id = sender()->property("id").toInt();
	int trIdx = sender()->property("transition_index").toInt();
	bool fadeToBlack = sender()->property("fadeToBlack").value<bool>();
	QuickTransition *qt = GetQuickTransition(id);

	if (qt) {
		OBSSource tr = fadeToBlack
				       ? OBSSource(fadeTransition)
				       : GetTransitionComboItem(ui->transitions,
								trIdx);
		if (tr) {
			qt->source = tr;
			ResetQuickTransitionText(qt);
		}
	}
}

void OBSBasic::QuickTransitionChangeDuration(int value)
{
	int id = sender()->property("id").toInt();
	QuickTransition *qt = GetQuickTransition(id);

	if (qt) {
		qt->duration = value;
		ResetQuickTransitionText(qt);
	}
}

void OBSBasic::QuickTransitionRemoveClicked()
{
	int id = sender()->property("id").toInt();
	int idx = GetQuickTransitionIdx(id);
	if (idx == -1)
		return;

	QuickTransition &qt = quickTransitions[idx];

	if (qt.button)
		qt.button->deleteLater();

	RemoveQuickTransitionHotkey(&qt);
	quickTransitions.erase(quickTransitions.begin() + idx);
}

void OBSBasic::ClearQuickTransitionWidgets()
{
	if (!IsPreviewProgramMode())
		return;

	QVBoxLayout *programLayout =
		reinterpret_cast<QVBoxLayout *>(programOptions->layout());

	for (int idx = 0;; idx++) {
		QLayoutItem *item = programLayout->itemAt(idx);
		if (!item)
			break;

		QWidget *widget = item->widget();
		if (!widget)
			continue;

		int id = widget->property("id").toInt();
		if (id != 0) {
			delete widget;
			idx--;
		}
	}
}

void OBSBasic::RefreshQuickTransitions()
{
	if (!IsPreviewProgramMode())
		return;

	for (QuickTransition &qt : quickTransitions)
		AddQuickTransitionId(qt.id);
}

void OBSBasic::EnableTransitionWidgets(bool enable)
{
	ui->transitions->setEnabled(enable);

	if (!enable) {
		ui->transitionProps->setEnabled(false);
	} else {
		bool configurable =
			obs_source_configurable(GetCurrentTransition());
		ui->transitionProps->setEnabled(configurable);
	}

	if (!IsPreviewProgramMode())
		return;

	QVBoxLayout *programLayout =
		reinterpret_cast<QVBoxLayout *>(programOptions->layout());

	for (int idx = 0;; idx++) {
		QLayoutItem *item = programLayout->itemAt(idx);
		if (!item)
			break;

		QPushButton *button =
			qobject_cast<QPushButton *>(item->widget());
		if (!button)
			continue;

		button->setEnabled(enable);
	}

	if (transitionButton)
		transitionButton->setEnabled(enable);
}

void OBSBasic::SetPreviewProgramMode(bool enabled)
{
	if (IsPreviewProgramMode() == enabled)
		return;

	ui->modeSwitch->setChecked(enabled);
	os_atomic_set_bool(&previewProgramMode, enabled);

	if (IsPreviewProgramMode()) {
		if (!previewEnabled)
			EnablePreviewDisplay(true);

		CreateProgramDisplay();
		CreateProgramOptions();

		OBSScene curScene = GetCurrentScene();

		OBSSceneAutoRelease dup;
		if (sceneDuplicationMode) {
			dup = obs_scene_duplicate(
				curScene,
				obs_source_get_name(
					obs_scene_get_source(curScene)),
				editPropertiesMode
					? OBS_SCENE_DUP_PRIVATE_COPY
					: OBS_SCENE_DUP_PRIVATE_REFS);
		} else {
			dup = std::move(OBSScene(curScene));
		}

		OBSSourceAutoRelease transition = obs_get_output_source(0);
		obs_source_t *dup_source = obs_scene_get_source(dup);
		obs_transition_set(transition, dup_source);

		if (curScene) {
			obs_source_t *source = obs_scene_get_source(curScene);
			obs_source_inc_showing(source);
			lastScene = OBSGetWeakRef(source);
			programScene = OBSGetWeakRef(source);
		}

		RefreshQuickTransitions();

		programLabel =
			new QLabel(QTStr("StudioMode.ProgramSceneLabel"), this);
		programLabel->setSizePolicy(QSizePolicy::Ignored,
					    QSizePolicy::Preferred);
		programLabel->setProperty("themeID", "previewProgramLabels");

		programWidget = new QWidget();
		programLayout = new QVBoxLayout();

		programLayout->setContentsMargins(0, 0, 0, 0);
		programLayout->setSpacing(0);

		programLayout->addWidget(programLabel);
		programLayout->addWidget(program);

		programWidget->setLayout(programLayout);

		ui->previewLayout->addWidget(programOptions);
		ui->previewLayout->addWidget(programWidget);
		ui->previewLayout->setAlignment(programOptions,
						Qt::AlignCenter);

		if (api)
			api->on_event(OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED);

		blog(LOG_INFO, "Switched to Preview/Program mode");
		blog(LOG_INFO, "-----------------------------"
			       "-------------------");
	} else {
		OBSSource actualProgramScene = OBSGetStrongRef(programScene);
		if (!actualProgramScene)
			actualProgramScene = GetCurrentSceneSource();
		else
			SetCurrentScene(actualProgramScene, true);
		TransitionToScene(actualProgramScene, true);

		delete programOptions;
		delete program;
		delete programLabel;
		delete programWidget;

		if (lastScene) {
			OBSSource actualLastScene = OBSGetStrongRef(lastScene);
			if (actualLastScene)
				obs_source_dec_showing(actualLastScene);
			lastScene = nullptr;
		}

		programScene = nullptr;
		swapScene = nullptr;
		prevFTBSource = nullptr;

		for (QuickTransition &qt : quickTransitions)
			qt.button = nullptr;

		if (!previewEnabled)
			EnablePreviewDisplay(false);

		ui->transitions->setEnabled(true);
		tBarActive = false;

		if (api)
			api->on_event(OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED);

		blog(LOG_INFO, "Switched to regular Preview mode");
		blog(LOG_INFO, "-----------------------------"
			       "-------------------");
	}

	ResetUI();
	UpdateTitleBar();
}

void OBSBasic::RenderProgram(void *data, uint32_t cx, uint32_t cy)
{
	GS_DEBUG_MARKER_BEGIN(GS_DEBUG_COLOR_DEFAULT, "RenderProgram");

	OBSBasic *window = static_cast<OBSBasic *>(data);
	obs_video_info ovi;

	obs_get_video_info(&ovi);

	window->programCX = int(window->programScale * float(ovi.base_width));
	window->programCY = int(window->programScale * float(ovi.base_height));

	gs_viewport_push();
	gs_projection_push();

	/* --------------------------------------- */

	gs_ortho(0.0f, float(ovi.base_width), 0.0f, float(ovi.base_height),
		 -100.0f, 100.0f);
	gs_set_viewport(window->programX, window->programY, window->programCX,
			window->programCY);

	obs_render_main_texture_src_color_only();
	gs_load_vertexbuffer(nullptr);

	/* --------------------------------------- */

	gs_projection_pop();
	gs_viewport_pop();

	GS_DEBUG_MARKER_END();

	UNUSED_PARAMETER(cx);
	UNUSED_PARAMETER(cy);
}

void OBSBasic::ResizeProgram(uint32_t cx, uint32_t cy)
{
	QSize targetSize;

	/* resize program panel to fix to the top section of the window */
	targetSize = GetPixelSize(program);
	GetScaleAndCenterPos(int(cx), int(cy),
			     targetSize.width() - PREVIEW_EDGE_SIZE * 2,
			     targetSize.height() - PREVIEW_EDGE_SIZE * 2,
			     programX, programY, programScale);

	programX += float(PREVIEW_EDGE_SIZE);
	programY += float(PREVIEW_EDGE_SIZE);
}

obs_data_array_t *OBSBasic::SaveTransitions()
{
	obs_data_array_t *transitions = obs_data_array_create();

	for (int i = 0; i < ui->transitions->count(); i++) {
		OBSSource tr = ui->transitions->itemData(i).value<OBSSource>();
		if (!tr || !obs_source_configurable(tr))
			continue;

		OBSDataAutoRelease sourceData = obs_data_create();
		OBSDataAutoRelease settings = obs_source_get_settings(tr);

		obs_data_set_string(sourceData, "name",
				    obs_source_get_name(tr));
		obs_data_set_string(sourceData, "id", obs_obj_get_id(tr));
		obs_data_set_obj(sourceData, "settings", settings);

		obs_data_array_push_back(transitions, sourceData);
	}

	return transitions;
}

void OBSBasic::LoadTransitions(obs_data_array_t *transitions,
			       obs_load_source_cb cb, void *private_data)
{
	size_t count = obs_data_array_count(transitions);

	for (size_t i = 0; i < count; i++) {
		OBSDataAutoRelease item = obs_data_array_item(transitions, i);
		const char *name = obs_data_get_string(item, "name");
		const char *id = obs_data_get_string(item, "id");
		OBSDataAutoRelease settings =
			obs_data_get_obj(item, "settings");

		OBSSourceAutoRelease source =
			obs_source_create_private(id, name, settings);
		if (!obs_obj_invalid(source)) {
			InitTransition(source);

			ui->transitions->addItem(
				QT_UTF8(name),
				QVariant::fromValue(OBSSource(source)));
			ui->transitions->setCurrentIndex(
				ui->transitions->count() - 1);
			if (cb)
				cb(private_data, source);
		}
	}
}

OBSSource OBSBasic::GetOverrideTransition(OBSSource source)
{
	if (!source)
		return nullptr;

	OBSDataAutoRelease data = obs_source_get_private_settings(source);

	const char *trOverrideName = obs_data_get_string(data, "transition");

	OBSSource trOverride = nullptr;

	if (trOverrideName && *trOverrideName)
		trOverride = FindTransition(trOverrideName);

	return trOverride;
}

int OBSBasic::GetOverrideTransitionDuration(OBSSource source)
{
	if (!source)
		return 300;

	OBSDataAutoRelease data = obs_source_get_private_settings(source);
	obs_data_set_default_int(data, "transition_duration", 300);

	return (int)obs_data_get_int(data, "transition_duration");
}

void OBSBasic::UpdatePreviewProgramIndicators()
{
	bool labels = previewProgramMode ? config_get_bool(GetGlobalConfig(),
							   "BasicWindow",
							   "StudioModeLabels")
					 : false;

	ui->previewLabel->setVisible(labels);

	if (programLabel)
		programLabel->setVisible(labels);

	if (!labels)
		return;

	QString preview = QTStr("StudioMode.PreviewSceneName")
				  .arg(QT_UTF8(obs_source_get_name(
					  GetCurrentSceneSource())));

	QString program =
		QTStr("StudioMode.ProgramSceneName")
			.arg(QT_UTF8(obs_source_get_name(GetProgramSource())));

	if (ui->previewLabel->text() != preview)
		ui->previewLabel->setText(preview);

	if (programLabel && programLabel->text() != program)
		programLabel->setText(program);
}
