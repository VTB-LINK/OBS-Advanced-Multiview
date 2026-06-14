/*
OBS Advanced Multiview - Cell context menu + window-level UI handlers

Split out of multiview-window.cpp for maintainability. All functions
remain members of MultiviewWindow.

Covers right-click cell menu (Add/Change/Edit/Clear/Reconnect/Replay/
Previous/Play-Pause/Next), Edit Grid, Global Settings, Fullscreen
toggle, Always-on-top toggle, and the underlying mouse-press hit
testing.

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#include "multiview-window.hpp"
#include "cell-display-settings-dialog.hpp"
#include "config-manager.hpp"
#include "edit-source-dialog.hpp"
#include "layout-engine.hpp"
#include "manager-dialog.hpp"
#include "signal-lost-settings-dialog.hpp"
#include "signal-provider.hpp"
#include "source-picker.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs.hpp>
#include <plugin-support.h>

#include <QAction>
#include <QApplication>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QScreen>
#include <QSize>
#include <QTimer>

#ifdef _WIN32
#include <windows.h>
#endif

/* Helper used by mousePressEvent below: lookup widget pixel size matching
 * the device pixel ratio. Mirrors the inline GetPixelSize() in the main
 * window TU. */
static QSize GetPixelSize(QWidget *w)
{
	const qreal dpr = w->devicePixelRatioF();
	return QSize(qRound((qreal)w->width() * dpr), qRound((qreal)w->height() * dpr));
}
void MultiviewWindow::mousePressEvent(QMouseEvent *event)
{
	if (event->button() == Qt::RightButton) {
		/* Hit test to determine which cell was clicked */
		QSize pixelSize = GetPixelSize(this);
		float ratio = (float)pixelSize.width() / (float)width();
		int mx = (int)(event->position().x() * ratio);
		int my = (int)(event->position().y() * ratio);

		/* Account for canvas aspect ratio viewport offset */
		int totalW = pixelSize.width();
		int totalH = pixelSize.height();
		double windowAspect = (double)totalW / (double)totalH;
		int vpX = 0, vpY = 0;
		int vpW = totalW, vpH = totalH;

		if (windowAspect > canvas_aspect_) {
			vpW = (int)((double)totalH * canvas_aspect_);
			vpX = (totalW - vpW) / 2;
		} else if (windowAspect < canvas_aspect_) {
			vpH = (int)((double)totalW / canvas_aspect_);
			vpY = (totalH - vpH) / 2;
		}

		/* Translate mouse to layout-local coordinates */
		int lx = mx - vpX;
		int ly = my - vpY;

		engine_.set_layout(layout_);
		engine_.set_viewport(vpW, vpH);
		engine_.compute();

		auto hit = engine_.hit_test(lx, ly);
		int cellIndex = -1;
		if (hit && hit->type == HitType::Cell)
			cellIndex = hit->cellIndex;

		show_context_menu(event->globalPosition().toPoint(), cellIndex);
	}

	QWidget::mousePressEvent(event);
}

/* ---- Context Menu ---- */

void MultiviewWindow::show_context_menu(const QPoint &pos, int cellIndex)
{
	QMenu menu(this);

	/* Phase 3 / M6.6 H.5: Plan B layout.
	 *
	 * Sections (top -> bottom), with separators between:
	 *
	 *   1. Window / instance state
	 *      - Fullscreen, Always on Top, Safe Area, VU Meter.
	 *      - Always shown regardless of cell hit.
	 *
	 *   2. Cell display & lost-signal config (cell hit only)
	 *      - Cell Display Settings..., Signal Lost Settings...
	 *      - Non-destructive, same family as Safe Area / VU Meter
	 *        above so the visual props all live in the upper half.
	 *
	 *   3. Source binding (cell hit only)
	 *      - Add Source... (empty cell)
	 *      - Change Source... / Edit Source... (external only) /
	 *        Clear Cell (occupied cell)
	 *      - Group destructive 'Clear Cell' with the binding-mutation
	 *        operations so it is not adjacent to non-destructive
	 *        config above.
	 *
	 *   4. Playback / reconnect (cell hit + occupied + provider supports)
	 *      - Replay Now / Reconnect Now
	 *      - Previous (VLC), Play / Pause (VLC + ffmpeg local), Next (VLC)
	 *
	 *   5. Window actions (always)
	 *      - Edit Grid... / Global Settings / Close
	 *
	 * Non-cell hits (gutter / border / outside grid): sections 2 / 3 / 4
	 * collapse to nothing, the menu shrinks to sections 1 + 5. */

	/* ---------- Section 1: window / instance ---------- */

	QAction *fullscreenAction = menu.addAction(QStringLiteral("Fullscreen"));
	fullscreenAction->setCheckable(true);
	fullscreenAction->setChecked(isFullScreen());
	connect(fullscreenAction, &QAction::triggered, this, &MultiviewWindow::on_toggle_fullscreen);

	QAction *onTopAction = menu.addAction(QStringLiteral("Always on Top"));
	onTopAction->setCheckable(true);
	onTopAction->setChecked(is_always_on_top_);
	connect(onTopAction, &QAction::triggered, this, &MultiviewWindow::on_toggle_always_on_top);

	{
		MultiviewInstance *inst = config_->find_instance(uuid_);
		if (inst) {
			bool safeEnabled = false;
			if (inst->visualSettings.safeAreaMode == InheritanceMode::Override)
				safeEnabled = inst->visualSettings.safeArea.enabled;
			else
				safeEnabled = config_->global_settings().visualSettings.safeArea.enabled;

			QAction *safeAreaAction = menu.addAction(QStringLiteral("Safe Area"));
			safeAreaAction->setCheckable(true);
			safeAreaAction->setChecked(safeEnabled);
			connect(safeAreaAction, &QAction::triggered, this, [this, safeEnabled]() {
				MultiviewInstance *inst = config_->find_instance(uuid_);
				if (!inst)
					return;
				inst->visualSettings.safeAreaMode = InheritanceMode::Override;
				inst->visualSettings.safeArea.enabled = !safeEnabled;
				config_->save();
				refresh_visual_settings();
			});
		}
	}

	{
		MultiviewInstance *inst = config_->find_instance(uuid_);
		if (inst) {
			bool vuEnabled = false;
			if (inst->visualSettings.vuMeterMode == InheritanceMode::Override)
				vuEnabled = inst->visualSettings.vuMeter.enabled;
			else
				vuEnabled = config_->global_settings().visualSettings.vuMeter.enabled;

			QAction *vuAction = menu.addAction(QStringLiteral("VU Meter"));
			vuAction->setCheckable(true);
			vuAction->setChecked(vuEnabled);
			connect(vuAction, &QAction::triggered, this, [this, vuEnabled]() {
				MultiviewInstance *inst = config_->find_instance(uuid_);
				if (!inst)
					return;
				inst->visualSettings.vuMeterMode = InheritanceMode::Override;
				inst->visualSettings.vuMeter.enabled = !vuEnabled;
				config_->save();
				refresh_visual_settings();
			});
		}
	}

	/* ---------- Section 2: cell display & lost (cell hit only) ---------- */

	if (cellIndex >= 0) {
		menu.addSeparator();

		QAction *cellSettingsAction = menu.addAction(QStringLiteral("Cell Display Settings..."));
		connect(cellSettingsAction, &QAction::triggered, this, [this, cellIndex]() {
			MultiviewInstance *inst = config_->find_instance(uuid_);
			if (!inst)
				return;

			LayoutEngine tmpEngine;
			tmpEngine.set_layout(inst->layout);
			tmpEngine.set_viewport(100, 100);
			tmpEngine.compute();
			const auto &cells = tmpEngine.cells();
			if (cellIndex >= (int)cells.size())
				return;
			int row = cells[cellIndex].gridRow;
			int col = cells[cellIndex].gridCol;

			CellVisualSettings *cvs = nullptr;
			for (auto &c : inst->cellVisualSettings) {
				if (c.row == row && c.col == col) {
					cvs = &c;
					break;
				}
			}
			CellVisualSettings temp;
			if (!cvs) {
				temp.row = row;
				temp.col = col;
				cvs = &temp;
			}

			CellDisplaySettingsDialog dlg(CellDisplaySettingsDialog::Mode::Cell, this);
			dlg.set_cell_position(row, col);
			{
				std::lock_guard<std::recursive_mutex> lock(source_mutex_);
				if (cellIndex >= 0 && cellIndex < (int)cell_sources_.size()) {
					const auto &cs = cell_sources_[cellIndex];
					dlg.set_external_cell(cs.provider_type != SignalProviderType::Unknown &&
							      !signal_provider_is_internal(cs.provider_type));
				}
			}
			dlg.set_cell_settings(*cvs);
			if (dlg.exec() == QDialog::Accepted) {
				CellVisualSettings result = dlg.get_cell_settings();
				result.row = row;
				result.col = col;

				bool found = false;
				for (auto &c : inst->cellVisualSettings) {
					if (c.row == row && c.col == col) {
						c = result;
						found = true;
						break;
					}
				}
				if (!found)
					inst->cellVisualSettings.push_back(result);

				config_->save();
				refresh_visual_settings();
			}
		});

		/* Phase 3 / M5.2: Cell-scoped Signal Lost Settings entry. Kept as a
		 * separate dialog from Cell Display Settings to honor the design
		 * doc §9 split between visual config and runtime/strategy config. */
		QAction *signalLostAction = menu.addAction(QStringLiteral("Signal Lost Settings..."));
		connect(signalLostAction, &QAction::triggered, this, [this, cellIndex]() {
			MultiviewInstance *inst = config_->find_instance(uuid_);
			if (!inst)
				return;

			LayoutEngine tmpEngine;
			tmpEngine.set_layout(inst->layout);
			tmpEngine.set_viewport(100, 100);
			tmpEngine.compute();
			const auto &cells = tmpEngine.cells();
			if (cellIndex >= (int)cells.size())
				return;
			int row = cells[cellIndex].gridRow;
			int col = cells[cellIndex].gridCol;

			CellLostSignalSettings working;
			if (const CellLostSignalSettings *existing = inst->find_cell_lost_signal(row, col)) {
				working = *existing;
			} else {
				working.row = row;
				working.col = col;
				working.mode = InheritanceMode::Inherit;
				working.settings = config_->global_settings().lostSignal;
			}

			SignalLostSettingsDialog dlg(SignalLostSettingsDialog::Mode::Cell, this);
			dlg.set_cell_position(row, col);
			dlg.set_cell_settings(working);
			if (dlg.exec() != QDialog::Accepted)
				return;

			CellLostSignalSettings result = dlg.get_cell_settings();
			result.row = row;
			result.col = col;

			bool found = false;
			for (auto &c : inst->cellLostSignalSettings) {
				if (c.row == row && c.col == col) {
					c = result;
					found = true;
					break;
				}
			}
			if (!found)
				inst->cellLostSignalSettings.push_back(result);

			config_->save();
			notify_multiview_signal_settings_changed(uuid_);
		});
	}

	/* ---------- Section 3: source binding (cell hit only) ---------- */

	if (cellIndex >= 0) {
		bool hasSource = false;
		bool isExternal = false;
		{
			std::lock_guard<std::recursive_mutex> lock(source_mutex_);
			if (cellIndex < (int)cell_sources_.size()) {
				const auto &cs = cell_sources_[cellIndex];
				/* Phase 3 / M6.1: external-provider cells leave
				 * cs.type empty (type/name are the legacy internal
				 * binding fields). Treat the cell as occupied if
				 * either an internal type or an external provider
				 * is set so the menu shows Change/Clear/Reconnect
				 * instead of Add Source. */
				if (!cs.type.empty() || cs.provider_type != SignalProviderType::Unknown)
					hasSource = true;
				if (cs.provider_type != SignalProviderType::Unknown &&
				    !signal_provider_is_internal(cs.provider_type))
					isExternal = true;
			}
		}

		menu.addSeparator();

		if (hasSource) {
			QAction *changeAction = menu.addAction(QStringLiteral("Change Source..."));
			connect(changeAction, &QAction::triggered, this,
				[this, cellIndex]() { on_change_source(cellIndex); });

			/* Phase 3 / M6.1+ task 9.1.C: Edit Source... shown only
			 * for external-provider cells. Internal cells already
			 * have Change Source... covering the same surface; a
			 * second entry would be confusing. */
			if (isExternal) {
				QAction *editAction = menu.addAction(QStringLiteral("Edit Source..."));
				connect(editAction, &QAction::triggered, this,
					[this, cellIndex]() { on_edit_source(cellIndex); });
			}

			QAction *clearAction = menu.addAction(QStringLiteral("Clear Cell"));
			connect(clearAction, &QAction::triggered, this,
				[this, cellIndex]() { on_clear_cell(cellIndex); });

			/* ---------- Section 4: playback / reconnect ---------- */

			/* Phase 3 / M5.3: Reconnect Now is enabled only when the cell is
			 * not already happily Active. Keeps the menu clean for steady
			 * cells and matches the "manual reconnect cooldown" semantics
			 * defined in [docs/phase-3-signal-lost-and-external-sources-design.md] §7.4.
			 *
			 * Phase 3 / M6.1+ polish: external local-file cells use
			 * "Replay Now" instead of "Reconnect Now" \u2014 there is no
			 * connection to re-establish, the action just restarts
			 * media playback (obs_source_media_restart).
			 *
			 * For external cells, expose the action even when state is
			 * Active so the user can manually replay a finished file
			 * or kick a stuck stream without waiting for the runtime
			 * to flip to Lost. The cooldown still throttles abuse. */
			bool canReconnect = false;
			bool isExternalLocalFile = false;
			bool isVlcCell = false;
			bool isFfmpegLocalFileCell = false;
			OBSSource vlcSourceSnapshot;
			OBSSource ffmpegLocalSourceSnapshot;
			{
				std::lock_guard<std::recursive_mutex> lock(source_mutex_);
				if (cellIndex < (int)cell_sources_.size()) {
					const auto &cs = cell_sources_[cellIndex];
					const bool isExternalForReconnect =
						cs.provider_type != SignalProviderType::Unknown &&
						!signal_provider_is_internal(cs.provider_type);
					canReconnect =
						cs.state == SignalRuntimeState::MissingInternal ||
						cs.state == SignalRuntimeState::Lost ||
						cs.state == SignalRuntimeState::Connecting ||
						cs.state == SignalRuntimeState::RetryScheduled ||
						cs.state == SignalRuntimeState::FallbackActive ||
						cs.state == SignalRuntimeState::Error ||
						(isExternalForReconnect && cs.state == SignalRuntimeState::Active);

					if (isExternalForReconnect && cs.private_source) {
						obs_data_t *cur = obs_source_get_settings(cs.private_source);
						if (cur) {
							isExternalLocalFile = obs_data_get_bool(cur, "is_local_file");
							obs_data_release(cur);
						}
					}

					if (cs.provider_type == SignalProviderType::Vlc) {
						isVlcCell = true;
						/* Snapshot the source under the lock so the
						 * media_* calls below can run without holding
						 * source_mutex_ (those calls go through libVLC
						 * and we don't want lock inversion). */
						vlcSourceSnapshot = cs.private_source;
					}

					/* Phase 3 / M6.4: FFmpeg local-file cells get a
					 * Play/Pause action too. Network streams (RTMP/HLS/
					 * SRT/...) deliberately don't \u2014 ffmpeg_source's
					 * pause behavior on a live network stream is
					 * effectively "stall the decoder, then race a
					 * reconnect when unpaused", which produces a
					 * confusing UX (cell freezes for a while, then
					 * jumps to whatever the stream is doing now).
					 * Replay Now still covers the use case of kicking
					 * a stuck network stream. */
					if (cs.provider_type == SignalProviderType::Ffmpeg && isExternalLocalFile) {
						isFfmpegLocalFileCell = true;
						ffmpegLocalSourceSnapshot = cs.private_source;
					}
				}
			}

			/* Section 4 separator. Only emit if at least one playback /
			 * reconnect action will be added; otherwise an empty section
			 * leaves a cosmetic dangling separator before Edit Grid. */
			const bool hasPlaybackSection = true; /* Reconnect/Replay always shown when hasSource */
			if (hasPlaybackSection)
				menu.addSeparator();

			/* VLC has no "connection" to re-establish either \u2014 the
			 * action restarts the current playlist entry via
			 * obs_source_media_restart, semantically identical to a
			 * local-file FFmpeg replay. */
			const bool useReplayLabel = isExternalLocalFile || isVlcCell;
			QAction *reconnectAction = menu.addAction(useReplayLabel ? QStringLiteral("Replay Now")
										 : QStringLiteral("Reconnect Now"));
			reconnectAction->setEnabled(canReconnect);
			connect(reconnectAction, &QAction::triggered, this,
				[this, cellIndex]() { (void)force_reconnect_cell(cellIndex); });

			/* Phase 3 / M6.4 playlist navigation: VLC is the only
			 * provider that exposes a real playlist (FFmpeg is one
			 * file/URL; NDI/Spout have no playlist concept). Wire
			 * Previous / Play-Pause / Next directly to
			 * obs_source_media_* which vlc_source registers via
			 * media_previous / media_play_pause / media_next. */
			if (isVlcCell && vlcSourceSnapshot) {
				QAction *prevAction = menu.addAction(QStringLiteral("Previous"));
				QAction *playPauseAction = menu.addAction(QStringLiteral("Play / Pause"));
				QAction *nextAction = menu.addAction(QStringLiteral("Next"));
				const obs_media_state st = obs_source_media_get_state(vlcSourceSnapshot);
				const bool currentlyPlaying = (st == OBS_MEDIA_STATE_PLAYING);
				connect(prevAction, &QAction::triggered, this, [src = vlcSourceSnapshot]() {
					if (src)
						obs_source_media_previous(src);
				});
				connect(playPauseAction, &QAction::triggered, this,
					[src = vlcSourceSnapshot, wasPlaying = currentlyPlaying]() {
						if (src)
							obs_source_media_play_pause(src, wasPlaying);
					});
				connect(nextAction, &QAction::triggered, this, [src = vlcSourceSnapshot]() {
					if (src)
						obs_source_media_next(src);
				});
			} else if (isFfmpegLocalFileCell && ffmpegLocalSourceSnapshot) {
				/* FFmpeg has no playlist, so only Play/Pause is
				 * meaningful (and only on local files \u2014 see the
				 * snapshot site for why network streams skip this). */
				QAction *playPauseAction = menu.addAction(QStringLiteral("Play / Pause"));
				const obs_media_state st = obs_source_media_get_state(ffmpegLocalSourceSnapshot);
				const bool currentlyPlaying = (st == OBS_MEDIA_STATE_PLAYING);
				connect(playPauseAction, &QAction::triggered, this,
					[src = ffmpegLocalSourceSnapshot, wasPlaying = currentlyPlaying]() {
						if (src)
							obs_source_media_play_pause(src, wasPlaying);
					});
			}
		} else {
			/* Empty cell: Add Source. No playback section follows. */
			QAction *addAction = menu.addAction(QStringLiteral("Add Source..."));
			connect(addAction, &QAction::triggered, this,
				[this, cellIndex]() { on_add_source(cellIndex); });
		}
	}

	/* ---------- Section 5: window-level actions (always) ---------- */

	menu.addSeparator();

	QAction *editGridAction = menu.addAction(QStringLiteral("Edit Grid..."));
	connect(editGridAction, &QAction::triggered, this, &MultiviewWindow::on_edit_grid);

	menu.addSeparator();

	QAction *settingsAction = menu.addAction(QStringLiteral("Global Settings"));
	connect(settingsAction, &QAction::triggered, this, &MultiviewWindow::on_global_settings);

	QAction *closeAction = menu.addAction(QStringLiteral("Close"));
	connect(closeAction, &QAction::triggered, this, &QWidget::close);

	menu.exec(pos);
}

void MultiviewWindow::on_add_source(int cellIndex)
{
	SourcePicker picker(this);
	if (picker.exec() != QDialog::Accepted)
		return;

	CellAssignment ca = picker.result_assignment();

	MultiviewInstance *inst = config_->find_instance(uuid_);
	if (!inst)
		return;

	/* Determine the (row, col) of the clicked cell from the engine */
	int r, c;
	{
		LayoutEngine tmpEngine;
		tmpEngine.set_layout(layout_);
		tmpEngine.set_viewport(cached_vpW_ > 0 ? cached_vpW_ : 800, cached_vpH_ > 0 ? cached_vpH_ : 600);
		tmpEngine.compute();
		const auto &cells = tmpEngine.cells();
		if (cellIndex < 0 || cellIndex >= (int)cells.size())
			return;
		r = cells[cellIndex].gridRow;
		c = cells[cellIndex].gridCol;
	}

	ca.row = r;
	ca.col = c;

	/* Replace existing assignment at (r,c) or add new */
	bool found = false;
	for (auto &a : inst->cellAssignments) {
		if (a.row == r && a.col == c) {
			a = ca;
			found = true;
			break;
		}
	}
	if (!found)
		inst->cellAssignments.push_back(ca);

	inst->signalDirty = true;
	config_->save();

	/* Phase 3 / M6.1+ task 9.1.A: try the single-cell incremental path
	 * first so other cells in the same window keep their external
	 * private sources alive. Falls back to refresh_sources() if cell
	 * count changed (shouldn't here — add/change does not affect cell
	 * count) or any other invariant slips. */
	if (!refresh_cell(r, c))
		refresh_sources();
}

void MultiviewWindow::on_change_source(int cellIndex)
{
	on_add_source(cellIndex); /* Same flow */
}

void MultiviewWindow::on_edit_source(int cellIndex)
{
	/* Phase 3 / M6.1+ task 9.1.C: Edit Source for external-provider
	 * cells. Opens the provider-specific form populated from the cell's
	 * current SignalConfig; on Save writes the new config back into
	 * the assignment and runs refresh_cell so other cells stay live.
	 *
	 * Internal cells (pgm/prvw/scene/source) never reach this entry
	 * because the menu hides Edit Source for them, but defend in depth
	 * against an unexpected dispatch path. */
	MultiviewInstance *inst = config_->find_instance(uuid_);
	if (!inst)
		return;

	/* Resolve (row, col) from cellIndex via the layout engine. */
	int r, c;
	{
		LayoutEngine tmpEngine;
		tmpEngine.set_layout(layout_);
		tmpEngine.set_viewport(cached_vpW_ > 0 ? cached_vpW_ : 800, cached_vpH_ > 0 ? cached_vpH_ : 600);
		tmpEngine.compute();
		const auto &cells = tmpEngine.cells();
		if (cellIndex < 0 || cellIndex >= (int)cells.size())
			return;
		r = cells[cellIndex].gridRow;
		c = cells[cellIndex].gridCol;
	}

	/* Find the matching CellAssignment and confirm it's external. */
	CellAssignment *target = nullptr;
	for (auto &a : inst->cellAssignments) {
		if (a.row == r && a.col == c) {
			target = &a;
			break;
		}
	}
	if (!target || !target->signalConfig.is_external())
		return;

	/* Snapshot the current signalConfig (deep copy via SignalConfig copy
	 * semantics so the dialog can edit a private copy without touching
	 * the live assignment). */
	SignalConfig snapshot = target->signalConfig;

	EditSourceDialog dlg(snapshot, this);
	if (dlg.exec() != QDialog::Accepted)
		return;

	SignalConfig new_cfg = dlg.signal_config();
	if (new_cfg.empty()) {
		/* Provider-specific form rejected validation post-Accept (rare;
		 * the dialog already shows a popup on its own). Bail without
		 * touching persisted state. */
		return;
	}

	/* Mutate, persist, then run the single-cell incremental refresh
	 * so neighboring external cells keep playing without interruption. */
	target->signalConfig = std::move(new_cfg);
	inst->signalDirty = true;
	config_->save();

	if (!refresh_cell(r, c))
		refresh_sources();
}

void MultiviewWindow::on_clear_cell(int cellIndex)
{
	MultiviewInstance *inst = config_->find_instance(uuid_);
	if (!inst)
		return;

	/* Determine (row, col) of the cell */
	int r, c;
	{
		LayoutEngine tmpEngine;
		tmpEngine.set_layout(layout_);
		tmpEngine.set_viewport(cached_vpW_ > 0 ? cached_vpW_ : 800, cached_vpH_ > 0 ? cached_vpH_ : 600);
		tmpEngine.compute();
		const auto &cells = tmpEngine.cells();
		if (cellIndex < 0 || cellIndex >= (int)cells.size())
			return;
		r = cells[cellIndex].gridRow;
		c = cells[cellIndex].gridCol;
	}

	/* Remove assignment at (r,c) */
	auto &assignments = inst->cellAssignments;
	assignments.erase(std::remove_if(assignments.begin(), assignments.end(),
					 [r, c](const CellAssignment &a) { return a.row == r && a.col == c; }),
			  assignments.end());

	inst->signalDirty = true;
	config_->save();

	/* Phase 3 / M6.1+ task 9.1.A: incremental path — the cleared cell's
	 * private source is released, the rest of the window untouched. */
	if (!refresh_cell(r, c))
		refresh_sources();
}

void MultiviewWindow::apply_clear_cell_for_rowcols(const std::vector<std::pair<int, int>> &rowCols)
{
	if (rowCols.empty())
		return;

	MultiviewInstance *inst = config_->find_instance(uuid_);
	if (!inst)
		return;

	auto &assignments = inst->cellAssignments;
	bool any_removed = false;
	for (const auto &rc : rowCols) {
		const int r = rc.first;
		const int c = rc.second;
		auto before = assignments.size();
		assignments.erase(std::remove_if(assignments.begin(), assignments.end(),
						 [r, c](const CellAssignment &a) { return a.row == r && a.col == c; }),
				  assignments.end());
		if (assignments.size() != before) {
			any_removed = true;
			obs_log(LOG_INFO, "ClearCell: dropped assignment at (row=%d, col=%d) for instance '%s'", r, c,
				inst->name.c_str());
		}
	}

	if (!any_removed)
		return;

	inst->signalDirty = true;
	config_->save();

	/* Phase 3 / M6.1+ task 9.1.A: incremental path per cleared cell.
	 * If the count somehow changed (shouldn't — ClearCell does not edit
	 * the layout grid), fall back to the heavy refresh once and break
	 * out so we don't run it for every entry in rowCols. */
	for (const auto &rc : rowCols) {
		if (!refresh_cell(rc.first, rc.second)) {
			refresh_sources();
			break;
		}
	}
}

void MultiviewWindow::on_save_assignments()
{
	MultiviewInstance *inst = config_->find_instance(uuid_);
	if (!inst)
		return;

	inst->signalDirty = false;
	config_->save();
	obs_log(LOG_INFO, "cell assignments saved for '%s'", inst->name.c_str());
}

void MultiviewWindow::on_edit_grid()
{
	open_manager_dialog();
}

void MultiviewWindow::on_global_settings()
{
	open_manager_dialog();
}

void MultiviewWindow::on_toggle_fullscreen()
{
	if (isFullScreen()) {
		showNormal();
	} else {
		showFullScreen();
	}
}

void MultiviewWindow::on_toggle_always_on_top()
{
	is_always_on_top_ = !is_always_on_top_;

#ifdef _WIN32
	/* Use SetWindowPos directly to avoid HWND recreation and flicker */
	HWND hwnd = (HWND)winId();
	HWND insertAfter = is_always_on_top_ ? HWND_TOPMOST : HWND_NOTOPMOST;
	SetWindowPos(hwnd, insertAfter, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
#else
	Qt::WindowFlags flags = windowFlags();
	if (is_always_on_top_)
		flags |= Qt::WindowStaysOnTopHint;
	else
		flags &= ~Qt::WindowStaysOnTopHint;

	destroy_display();
	setWindowFlags(flags);
	show();
	create_display();
#endif
}
