/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2013 DreamSourceLab <support@dreamsourcelab.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#ifndef PXVIEW_PV_VIEW_SIGNALFACTORY_H
#define PXVIEW_PV_VIEW_SIGNALFACTORY_H

#include <memory>

#include <vector>
#include <map>

namespace pv {

class SigSession;

namespace data {
class SignalModel;
class DataSource;
class LogicSnapshot;
class AnalogSnapshot;
class DsoSnapshot;
}

namespace view {

class Signal;
struct DockUiState;

/**
 * SignalFactory creates view::Signal objects from SignalModel.
 * It is the bridge between Core layer (SignalModel) and View layer (Signal).
 */
class SignalFactory
{
public:
    /**
     * Signal change event type for incremental updates.
     */
    enum SignalChangeEvent {
        Added,
        Removed,
        Modified,
        AllReplaced
    };

    /**
     * UI state for preserving across signal recreation.
     */
    struct SignalUiState {
        int channel_index;
        bool selected;
        bool visible;
        int view_index;
        int v_offset;
        int own_height;
    };

    /**
     * Create a single Signal from a SignalModel.
     * @param model The SignalModel to create from.
     * @param data_source The DataSource for data/snapshot access.
     * @return The created Signal, or nullptr if type is unknown.
     */
    static Signal* create_signal(std::shared_ptr<data::SignalModel> model, data::DataSource *data_source);

    /**
     * Create all signals from a DataSource's signal models.
     * @param source The DataSource to read signal models from.
     * @param data_source The DataSource for snapshot access (typically the
     *                    same object as source; kept as a separate param for
     *                    the SigSession->DataSource migration).
     * @return Vector of created Signal pointers.
     */
    static std::vector<std::unique_ptr<Signal>> create_signals(data::DataSource *source, data::DataSource *data_source);

    /**
     * Compute the change event type by comparing current signals with new models.
     * @param current_signals Existing Signal objects in View.
     * @param models New SignalModel objects from Core.
     * @return SignalChangeEvent indicating the type of change.
     */
    static SignalChangeEvent compute_change_event(
        const std::vector<std::unique_ptr<Signal>> &current_signals,
        const std::vector<std::shared_ptr<data::SignalModel>> &models);

    /**
     * Incrementally update existing signals based on signal models.
     * Preserves UI state (selected, visible, v_offset, etc.) for signals
     * that survive the update.
     * @param current_signals Current signal list (will be modified in place).
     * @param source The DataSource to read signal models from.
     * @param data_source The DataSource for snapshot access (typically the
     *                    same object as source).
     * @param event Type of change that triggered the update.
     */
    static void update_signals(std::vector<std::unique_ptr<Signal>> &current_signals,
                               data::DataSource *source,
                               data::DataSource *data_source,
                               SignalChangeEvent event);

private:
    /**
     * Save UI state from a list of signals.
     */
    static std::map<int, SignalUiState> save_ui_state(const std::vector<std::unique_ptr<Signal>> &sigs);

    /**
     * Restore UI state to signals based on channel index mapping.
     */
    static void restore_ui_state(std::vector<std::unique_ptr<Signal>> &sigs,
                                  const std::map<int, SignalUiState> &saved_state);

    /**
     * Get the snapshot for a signal type from the data source.
     */
    static data::LogicSnapshot* get_logic_snapshot(data::DataSource *data_source);
    static data::AnalogSnapshot* get_analog_snapshot(data::DataSource *data_source);
    static data::DsoSnapshot* get_dso_snapshot(data::DataSource *data_source);
};

} // namespace view
} // namespace pv

#endif // PXVIEW_PV_VIEW_SIGNALFACTORY_H
