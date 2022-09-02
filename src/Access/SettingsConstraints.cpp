#include <Access/SettingsConstraints.h>
#include <Access/AccessControl.h>
#include <Core/Settings.h>
#include <Common/FieldVisitorToString.h>
#include <Common/FieldVisitorsAccurateComparison.h>
#include <IO/WriteHelpers.h>
#include <Poco/Util/AbstractConfiguration.h>
#include <boost/range/algorithm_ext/erase.hpp>


namespace DB
{
namespace ErrorCodes
{
    extern const int READONLY;
    extern const int QUERY_IS_PROHIBITED;
    extern const int SETTING_CONSTRAINT_VIOLATION;
    extern const int UNKNOWN_SETTING;
}


SettingsConstraints::SettingsConstraints(const AccessControl & access_control_) : access_control(&access_control_)
{
}

SettingsConstraints::SettingsConstraints(const SettingsConstraints & src) = default;
SettingsConstraints & SettingsConstraints::operator=(const SettingsConstraints & src) = default;
SettingsConstraints::SettingsConstraints(SettingsConstraints && src) noexcept = default;
SettingsConstraints & SettingsConstraints::operator=(SettingsConstraints && src) noexcept = default;
SettingsConstraints::~SettingsConstraints() = default;


void SettingsConstraints::clear()
{
    constraints.clear();
}


void SettingsConstraints::setMinValue(const String & setting_name, const Field & min_value)
{
    constraints[setting_name].min_value = Settings::castValueUtil(setting_name, min_value);
}

void SettingsConstraints::setMaxValue(const String & setting_name, const Field & max_value)
{
    constraints[setting_name].max_value = Settings::castValueUtil(setting_name, max_value);
}

void SettingsConstraints::setIsConst(const String & setting_name, bool is_const)
{
    constraints[setting_name].is_const = is_const;
}

void SettingsConstraints::setChangableInReadonly(const String & setting_name, bool changeable_in_readonly)
{
    constraints[setting_name].changeable_in_readonly = changeable_in_readonly;
}

void SettingsConstraints::get(const Settings & current_settings, std::string_view setting_name, Field & min_value, Field & max_value, bool & is_const) const
{
    auto range = getRange(current_settings, setting_name);
    min_value = range.min_value;
    max_value = range.max_value;
    is_const = range.is_const;
}

void SettingsConstraints::merge(const SettingsConstraints & other)
{
    if (access_control.doesSettingsConstraintsReplacePrevious())
    {
        for (const auto & [other_name, other_constraint] : other.constraints)
            constraints[other_name] = other_constraint;
    }
    else
    {
        for (const auto & [other_name, other_constraint] : other.constraints)
        {
            auto & constraint = constraints[other_name];
            if (!other_constraint.min_value.isNull())
                constraint.min_value = other_constraint.min_value;
            if (!other_constraint.max_value.isNull())
                constraint.max_value = other_constraint.max_value;
            if (other_constraint.is_const)
                constraint.is_const = true; // NOTE: In this mode <readonly/> flag cannot be overriden to be false
        }
    }
}


void SettingsConstraints::check(const Settings & current_settings, const SettingChange & change) const
{
    checkImpl(current_settings, const_cast<SettingChange &>(change), THROW_ON_VIOLATION);
}

void SettingsConstraints::check(const Settings & current_settings, const SettingsChanges & changes) const
{
    for (const auto & change : changes)
        check(current_settings, change);
}

void SettingsConstraints::check(const Settings & current_settings, SettingsChanges & changes) const
{
    boost::range::remove_erase_if(
        changes,
        [&](SettingChange & change) -> bool
        {
            return !checkImpl(current_settings, const_cast<SettingChange &>(change), THROW_ON_VIOLATION);
        });
}

void SettingsConstraints::clamp(const Settings & current_settings, SettingsChanges & changes) const
{
    boost::range::remove_erase_if(
        changes,
        [&](SettingChange & change) -> bool
        {
            return !checkImpl(current_settings, change, CLAMP_ON_VIOLATION);
        });
}


bool SettingsConstraints::checkImpl(const Settings & current_settings, SettingChange & change, ReactionOnViolation reaction) const
{
    const String & setting_name = change.name;

    if (setting_name == "profile")
        return true;

    bool cannot_cast;
    auto cast_value = [&](const Field & x) -> Field
    {
        cannot_cast = false;
        if (reaction == THROW_ON_VIOLATION)
            return Settings::castValueUtil(setting_name, x);
        else
        {
            try
            {
                return Settings::castValueUtil(setting_name, x);
            }
            catch (...)
            {
                cannot_cast = true;
                return {};
            }
        }
    };

    if (reaction == THROW_ON_VIOLATION)
    {
        try
        {
            access_control->checkSettingNameIsAllowed(setting_name);
        }
        catch (Exception & e)
        {
            if (e.code() == ErrorCodes::UNKNOWN_SETTING)
            {
                if (const auto hints = current_settings.getHints(change.name); !hints.empty())
                {
                      e.addMessage(fmt::format("Maybe you meant {}", toString(hints)));
                }
            }
            throw;
        }
    }
    else if (!access_control->isSettingNameAllowed(setting_name))
        return false;

    Field current_value, new_value;
    if (current_settings.tryGet(setting_name, current_value))
    {
        /// Setting isn't checked if value has not changed.
        if (change.value == current_value)
            return false;

        new_value = cast_value(change.value);
        if ((new_value == current_value) || cannot_cast)
            return false;
    }
    else
    {
        new_value = cast_value(change.value);
        if (cannot_cast)
            return false;
    }

    return getRange(current_settings, setting_name).check(change, new_value, reaction);
}

bool SettingsConstraints::Range::check(SettingChange & change, const Field & new_value, ReactionOnViolation reaction) const
{
    const String & setting_name = change.name;

    auto less_or_cannot_compare = [=](const Field & left, const Field & right)
    {
        if (reaction == THROW_ON_VIOLATION)
            return applyVisitor(FieldVisitorAccurateLess{}, left, right);
        else
        {
            try
            {
                return applyVisitor(FieldVisitorAccurateLess{}, left, right);
            }
            catch (...)
            {
                return true;
            }
        }
    };

    if (!explain.empty())
    {
        if (reaction == THROW_ON_VIOLATION)
            throw Exception(explain, code);
        else
            return false;
    }

    if (is_const)
    {
        if (reaction == THROW_ON_VIOLATION)
            throw Exception("Setting " + setting_name + " should not be changed", ErrorCodes::SETTING_CONSTRAINT_VIOLATION);
        else
            return false;
    }

    if (!min_value.isNull() && !max_value.isNull() && less_or_cannot_compare(max_value, min_value))
    {
        if (reaction == THROW_ON_VIOLATION)
            throw Exception("Setting " + setting_name + " should not be changed", ErrorCodes::SETTING_CONSTRAINT_VIOLATION);
        else
            return false;
    }

    if (!min_value.isNull() && less_or_cannot_compare(new_value, min_value))
    {
        if (reaction == THROW_ON_VIOLATION)
        {
            throw Exception(
                "Setting " + setting_name + " shouldn't be less than " + applyVisitor(FieldVisitorToString(), min_value),
                ErrorCodes::SETTING_CONSTRAINT_VIOLATION);
        }
        else
            change.value = min_value;
    }

    if (!max_value.isNull() && less_or_cannot_compare(max_value, new_value))
    {
        if (reaction == THROW_ON_VIOLATION)
        {
            throw Exception(
                "Setting " + setting_name + " shouldn't be greater than " + applyVisitor(FieldVisitorToString(), max_value),
                ErrorCodes::SETTING_CONSTRAINT_VIOLATION);
        }
        else
            change.value = max_value;
    }

    return true;
}

SettingsConstraints::Range SettingsConstraints::getRange(const Settings & current_settings, std::string_view setting_name) const
{
    if (!current_settings.allow_ddl && setting_name == "allow_ddl")
        return Range::forbidden("Cannot modify 'allow_ddl' setting when DDL queries are prohibited for the user", ErrorCodes::QUERY_IS_PROHIBITED);

    /** The `readonly` value is understood as follows:
      * 0 - no read-only restrictions.
      * 1 - only read requests, as well as changing settings with `changable_in_readonly` flag.
      * 2 - only read requests, as well as changing settings, except for the `readonly` setting.
      */

    if (current_settings.readonly > 1 && setting_name == "readonly")
        return Range::forbidden("Cannot modify 'readonly' setting in readonly mode", ErrorCodes::READONLY);

    auto it = constraints.find(setting_name);
    if (current_settings.readonly == 1)
    {
        if (it == constraints.end() || !it->second.changeable_in_readonly)
            return Range::forbidden("Cannot modify '" + String(setting_name) + "' setting in readonly mode", ErrorCodes::READONLY);
    }
    else // For both readonly=0 and readonly=2
    {
        if (it == constraints.end())
            return Range::allowed();
    }
    return it->second;
}

bool SettingsConstraints::Range::operator==(const Range & other) const
{
    return is_const == other.is_const && changeable_in_readonly == other.changeable_in_readonly && min_value == other.min_value && max_value == other.max_value;
}

bool operator ==(const SettingsConstraints & left, const SettingsConstraints & right)
{
    return left.constraints == right.constraints;
}
}
