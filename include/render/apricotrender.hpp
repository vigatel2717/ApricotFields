//
// Created by Nathan on 5/12/2026.
//

#ifndef APRICOTFIELDS_APRICOTRENDER_HPP
#define APRICOTFIELDS_APRICOTRENDER_HPP

namespace Apricot::Fields {
	class Renderer {
	public:
		Renderer();
		~Renderer();

		void BeginScene();
		void EndScene();

		void Render();
	};
}

#endif //APRICOTFIELDS_APRICOTRENDER_HPP
