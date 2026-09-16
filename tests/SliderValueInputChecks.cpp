#include "UI/SliderValueInput.h"
#include <iostream>
#include <stdexcept>

using DietDrCamera::ParseSliderValue;

int main()
{
    try {
        const auto check = [](bool ok) { if (!ok) throw std::runtime_error("Slider numeric input failed"); };
        check(ParseSliderValue(" -0.125 ", -1.5f, 1.5f) == -0.125f);
        check(ParseSliderValue("+8.025e1", 60.0f, 120.0f) == 80.25f);
        check(ParseSliderValue("500", -100.0f, 100.0f) == 100.0f);
        check(ParseSliderValue("-500", -100.0f, 100.0f) == -100.0f);
        for (const auto* text : {"", " ", "-", ".", "+", "+-1", "++1", "1.2.3", "12junk", "nan", "inf", "-inf", "1e9999"})
            check(!ParseSliderValue(text, -100.0f, 100.0f));
        std::cout << "Slider numeric input checks passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
